#include "checkpoint.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cutwidth {
namespace {

// CWCP1 is an ASCII, newline-delimited format. Hashes are length-prefixed so
// their algorithm and alphabet may evolve without changing the file format.
// Numeric lists are comma separated. No whitespace, unknown fields, duplicate
// fields, or non-canonical numbers are accepted.
constexpr std::string_view magic = "CWCP1";

template <typename Integer>
Integer parse_integer(std::string_view text, std::string_view field) {
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
        throw std::invalid_argument("invalid checkpoint " + std::string(field));
    Integer value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("invalid checkpoint " + std::string(field));
    return value;
}

std::string parse_length_prefixed(std::string_view text, std::string_view field) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos)
        throw std::invalid_argument("invalid checkpoint " + std::string(field));
    const auto length = parse_integer<std::size_t>(text.substr(0, colon), field);
    const auto value = text.substr(colon + 1);
    if (value.size() != length || value.empty())
        throw std::invalid_argument("invalid checkpoint " + std::string(field));
    for (const unsigned char c : value) {
        if (c < 0x21 || c > 0x7e)
            throw std::invalid_argument("invalid checkpoint " + std::string(field));
    }
    return std::string(value);
}

std::vector<std::uint32_t> parse_ordering(std::string_view text) {
    std::vector<std::uint32_t> result;
    if (text.empty()) return result;
    while (true) {
        const auto comma = text.find(',');
        result.push_back(parse_integer<std::uint32_t>(text.substr(0, comma), "ordering"));
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (text.empty()) throw std::invalid_argument("invalid checkpoint ordering");
    }
    return result;
}

std::vector<CompletedThreshold> parse_thresholds(std::string_view text) {
    std::vector<CompletedThreshold> result;
    if (text.empty()) return result;
    while (true) {
        const auto comma = text.find(',');
        const auto item = text.substr(0, comma);
        const auto colon = item.find(':');
        if (colon == std::string_view::npos || colon + 2 != item.size())
            throw std::invalid_argument("invalid checkpoint completed_thresholds");
        const char code = item.back();
        if (code != 'F' && code != 'I')
            throw std::invalid_argument("invalid checkpoint completed_thresholds");
        result.push_back({parse_integer<std::uint32_t>(item.substr(0, colon),
                                                       "completed_thresholds"),
                          code == 'F' ? CompletedThresholdResult::feasible
                                      : CompletedThresholdResult::infeasible});
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
        if (text.empty())
            throw std::invalid_argument("invalid checkpoint completed_thresholds");
    }
    return result;
}

bool parse_bool(std::string_view text, std::string_view field) {
    if (text == "0") return false;
    if (text == "1") return true;
    throw std::invalid_argument("invalid checkpoint " + std::string(field));
}

std::string length_prefixed(const std::string& value) {
    return std::to_string(value.size()) + ':' + value;
}

} // namespace

void validate_checkpoint(const Checkpoint& checkpoint) {
    if (checkpoint.graph_hash.empty() || checkpoint.solver_hash.empty() ||
        checkpoint.options_hash.empty())
        throw std::invalid_argument("checkpoint hashes must not be empty");
    for (const auto* hash : {&checkpoint.graph_hash, &checkpoint.solver_hash,
                             &checkpoint.options_hash}) {
        if (hash->size() > 4096)
            throw std::invalid_argument("checkpoint hash identifier is too long");
        for (const unsigned char c : *hash) {
            if (c < 0x21 || c > 0x7e)
                throw std::invalid_argument("checkpoint hashes must be printable ASCII");
        }
    }
    if (checkpoint.lower_bound > checkpoint.upper_bound)
        throw std::invalid_argument("checkpoint lower bound exceeds upper bound");
    std::set<std::uint32_t> vertices;
    for (const auto vertex : checkpoint.ordering) {
        if (!vertices.insert(vertex).second)
            throw std::invalid_argument("checkpoint ordering contains a duplicate vertex");
    }
    std::set<std::uint32_t> thresholds;
    std::optional<std::uint32_t> previous_threshold;
    for (const auto& completed : checkpoint.completed_thresholds) {
        if (!thresholds.insert(completed.threshold).second)
            throw std::invalid_argument("checkpoint contains a duplicate threshold");
        if (previous_threshold && completed.threshold <= *previous_threshold)
            throw std::invalid_argument("checkpoint thresholds are not sorted");
        previous_threshold = completed.threshold;
        if (completed.result == CompletedThresholdResult::feasible &&
            checkpoint.upper_bound > completed.threshold)
            throw std::invalid_argument("checkpoint feasible threshold contradicts upper bound");
        if (completed.result == CompletedThresholdResult::infeasible) {
            if (completed.threshold == std::numeric_limits<std::uint32_t>::max() ||
                checkpoint.lower_bound < completed.threshold + 1)
                throw std::invalid_argument(
                    "checkpoint infeasible threshold contradicts lower bound");
        }
    }
}

std::string serialize_checkpoint(const Checkpoint& checkpoint) {
    validate_checkpoint(checkpoint);
    std::ostringstream out;
    out << magic << '\n'
        << "graph_hash=" << length_prefixed(checkpoint.graph_hash) << '\n'
        << "solver_hash=" << length_prefixed(checkpoint.solver_hash) << '\n'
        << "options_hash=" << length_prefixed(checkpoint.options_hash) << '\n'
        << "lower_bound=" << checkpoint.lower_bound << '\n'
        << "upper_bound=" << checkpoint.upper_bound << '\n'
        << "elapsed_ms=" << checkpoint.elapsed_milliseconds << '\n'
        << "timed_out=" << checkpoint.timed_out << '\n'
        << "interrupted=" << checkpoint.interrupted << '\n'
        << "ordering=";
    for (std::size_t i = 0; i < checkpoint.ordering.size(); ++i) {
        if (i) out << ',';
        out << checkpoint.ordering[i];
    }
    out << "\ncompleted_thresholds=";
    for (std::size_t i = 0; i < checkpoint.completed_thresholds.size(); ++i) {
        if (i) out << ',';
        const auto& value = checkpoint.completed_thresholds[i];
        out << value.threshold << ':'
            << (value.result == CompletedThresholdResult::feasible ? 'F' : 'I');
    }
    out << '\n';
    return out.str();
}

Checkpoint parse_checkpoint(const std::string& encoded) {
    if (encoded.empty() || encoded.back() != '\n')
        throw std::invalid_argument("checkpoint is truncated");
    std::istringstream input(encoded);
    std::string line;
    if (!std::getline(input, line) || line != magic)
        throw std::invalid_argument("unsupported checkpoint format");
    std::set<std::string> seen;
    Checkpoint result;
    while (std::getline(input, line)) {
        if (line.empty()) throw std::invalid_argument("empty checkpoint field");
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            throw std::invalid_argument("malformed checkpoint field");
        const auto key = line.substr(0, equals);
        const std::string_view value(line.data() + equals + 1, line.size() - equals - 1);
        if (!seen.insert(key).second)
            throw std::invalid_argument("duplicate checkpoint field: " + key);
        if (key == "graph_hash") result.graph_hash = parse_length_prefixed(value, key);
        else if (key == "solver_hash") result.solver_hash = parse_length_prefixed(value, key);
        else if (key == "options_hash") result.options_hash = parse_length_prefixed(value, key);
        else if (key == "lower_bound") result.lower_bound = parse_integer<std::uint32_t>(value, key);
        else if (key == "upper_bound") result.upper_bound = parse_integer<std::uint32_t>(value, key);
        else if (key == "elapsed_ms") result.elapsed_milliseconds = parse_integer<std::uint64_t>(value, key);
        else if (key == "timed_out") result.timed_out = parse_bool(value, key);
        else if (key == "interrupted") result.interrupted = parse_bool(value, key);
        else if (key == "ordering") result.ordering = parse_ordering(value);
        else if (key == "completed_thresholds") result.completed_thresholds = parse_thresholds(value);
        else throw std::invalid_argument("unknown checkpoint field: " + key);
    }
    constexpr std::size_t required_fields = 10;
    if (seen.size() != required_fields)
        throw std::invalid_argument("checkpoint is missing required fields");
    validate_checkpoint(result);
    // Enforce one canonical representation, detecting otherwise ambiguous or
    // subtly corrupted encodings as well as malformed values.
    if (serialize_checkpoint(result) != encoded)
        throw std::invalid_argument("checkpoint is not canonically encoded");
    return result;
}

Checkpoint read_checkpoint(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open checkpoint: " + path.string());
    constexpr std::uintmax_t maximum_checkpoint_bytes = 16U * 1024U * 1024U;
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (!size_error && size > maximum_checkpoint_bytes)
        throw std::runtime_error("checkpoint exceeds portable size limit");
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) throw std::runtime_error("cannot read checkpoint: " + path.string());
    return parse_checkpoint(contents.str());
}

void write_checkpoint_atomic(const std::filesystem::path& path,
                             const Checkpoint& checkpoint) {
    const auto encoded = serialize_checkpoint(checkpoint);
    if (path.empty() || path.filename().empty())
        throw std::invalid_argument("checkpoint path must name a file");
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
    static std::atomic<std::uint64_t> sequence{0};
#ifdef _WIN32
    const auto process_id = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto process_id = static_cast<unsigned long>(::getpid());
#endif
    const auto temporary = parent / (path.filename().string() + ".tmp." +
        std::to_string(process_id) + "." + std::to_string(sequence.fetch_add(1)));
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (!file) throw std::system_error(errno, std::generic_category(),
                                       "cannot create checkpoint temporary file");
    bool closed = false;
    try {
        if (std::fwrite(encoded.data(), 1, encoded.size(), file) != encoded.size() ||
            std::fflush(file) != 0)
            throw std::system_error(errno, std::generic_category(), "cannot write checkpoint");
#ifdef _WIN32
        if (_commit(_fileno(file)) != 0)
#else
        if (::fsync(fileno(file)) != 0)
#endif
            throw std::system_error(errno, std::generic_category(), "cannot sync checkpoint");
        if (std::fclose(file) != 0) {
            closed = true;
            throw std::system_error(errno, std::generic_category(), "cannot close checkpoint");
        }
        closed = true;
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "cannot replace checkpoint");
#else
        std::filesystem::rename(temporary, path);
#endif
#ifndef _WIN32
        const int directory = ::open(parent.string().c_str(), O_RDONLY);
        if (directory >= 0) {
            (void)::fsync(directory);
            (void)::close(directory);
        }
#endif
    } catch (...) {
        if (!closed) std::fclose(file);
        std::filesystem::remove(temporary, cleanup_error);
        throw;
    }
}

} // namespace cutwidth
