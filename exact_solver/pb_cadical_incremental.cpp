#include "pb_cadical_incremental.hpp"

#include <chrono>
#include <limits>
#include <filesystem>
#include <utility>
#include <sstream>
#include <iomanip>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#ifdef CUTWIDTH_HAVE_CADICAL
#include <cadical.hpp>
#endif

namespace cutwidth::pb {
namespace {
using Clock = std::chrono::steady_clock;

std::string fnv1a64_bytes(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x00000100000001B3ULL;
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

}

struct IncrementalCadicalSession::Impl {
    std::uint32_t current_threshold = 0;
    std::uint32_t variable_count = 0;
    std::vector<std::vector<std::uint32_t>> prefix_variables;
    std::vector<std::vector<std::uint32_t>> crossing_outputs;
    std::string diagnostic;
    std::string temporary_directory;
    std::string proof_path;
    std::vector<std::int32_t> added_units;
    bool proof_open = false;
    bool keep_temporary_files = false;
    std::vector<std::uint8_t> proof_bytes;
#ifdef CUTWIDTH_HAVE_CADICAL
    std::unique_ptr<InMemoryDratTracer> drat_tracer;
    struct DeadlineTerminator final : CaDiCaL::Terminator {
        Clock::time_point deadline = Clock::time_point::max();
        const InMemoryDratTracer* tracer = nullptr;
        bool terminate() override {
            return Clock::now() >= deadline || (tracer && tracer->overflowed());
        }
    } terminator;
    CaDiCaL::Solver solver;
#endif
};

IncrementalCadicalSession::IncrementalCadicalSession(
    const CutwidthCnf& encoding, std::uint32_t initial_threshold,
    const std::vector<Graph::Vertex>& phase_ordering,
    bool trace_proof, bool keep_temporary_files)
    : impl_(std::make_unique<Impl>()) {
    impl_->current_threshold = initial_threshold;
    impl_->variable_count = encoding.formula.variable_count;
    impl_->prefix_variables = encoding.prefix_variables;
    impl_->crossing_outputs = encoding.crossing_count_outputs;
    impl_->keep_temporary_files = keep_temporary_files;
#ifdef CUTWIDTH_HAVE_CADICAL
    impl_->solver.set("quiet", 1);
    if (trace_proof) {
        impl_->drat_tracer = std::make_unique<InMemoryDratTracer>(impl_->proof_bytes, true);
        impl_->terminator.tracer = impl_->drat_tracer.get();
        impl_->solver.connect_proof_tracer(impl_->drat_tracer.get(), false);
        impl_->proof_open = true;
    }
    for (const auto& clause : encoding.formula.clauses) {
        for (const auto literal : clause) impl_->solver.add(literal);
        impl_->solver.add(0);
    }
    if (phase_ordering.size() == encoding.vertex_count) {
        std::vector<std::size_t> position(encoding.vertex_count);
        bool valid = true;
        for (std::size_t index = 0; index < phase_ordering.size(); ++index) {
            if (phase_ordering[index] >= position.size()) { valid = false; break; }
            position[phase_ordering[index]] = index;
        }
        if (valid) {
            for (std::size_t vertex = 0; vertex < encoding.vertex_count; ++vertex)
                for (std::size_t prefix = 0;
                     prefix < encoding.prefix_variables[vertex].size(); ++prefix) {
                    const auto variable = static_cast<int>(
                        encoding.prefix_variables[vertex][prefix]);
                    impl_->solver.phase(position[vertex] <= prefix ? variable : -variable);
                }
        }
    }
#else
    (void)phase_ordering;
    (void)trace_proof;
    impl_->diagnostic = "pinned native CaDiCaL support was not compiled";
#endif
}

IncrementalCadicalSession::~IncrementalCadicalSession() {
#ifdef CUTWIDTH_HAVE_CADICAL
    if (impl_ && impl_->proof_open) {
        impl_->solver.disconnect_proof_tracer(impl_->drat_tracer.get());
        impl_->proof_open = false;
    }
#endif
    if (impl_ && !impl_->keep_temporary_files && !impl_->temporary_directory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(impl_->temporary_directory, ignored);
    }
}
bool IncrementalCadicalSession::available() const noexcept {
#ifdef CUTWIDTH_HAVE_CADICAL
    return true;
#else
    return false;
#endif
}

IncrementalResult IncrementalCadicalSession::solve(
    std::uint32_t threshold, std::chrono::milliseconds time_limit) {
    IncrementalResult result;
#ifndef CUTWIDTH_HAVE_CADICAL
    (void)threshold;
    (void)time_limit;
    result.diagnostic = impl_->diagnostic;
    return result;
#else
    if (threshold > impl_->current_threshold) {
        result.status = IncrementalStatus::error;
        result.diagnostic = "incremental CaDiCaL threshold cannot be loosened";
        return result;
    }
    if (threshold < impl_->current_threshold) {
        for (const auto& outputs : impl_->crossing_outputs) {
            if (threshold >= outputs.size()) {
                result.status = IncrementalStatus::error;
                result.diagnostic = "incremental crossing counter lacks requested threshold";
                return result;
            }
            impl_->solver.add(-static_cast<int>(outputs[threshold]));
            impl_->solver.add(0);
            impl_->added_units.push_back(-static_cast<std::int32_t>(outputs[threshold]));
        }
        impl_->current_threshold = threshold;
    }
    const auto started = Clock::now();
    impl_->terminator.deadline = time_limit.count() == 0
        ? Clock::time_point::max() : started + time_limit;
    impl_->solver.connect_terminator(&impl_->terminator);
    const int status = impl_->solver.solve();
    impl_->solver.disconnect_terminator();
    result.runtime_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (status == 10) {
        result.status = IncrementalStatus::sat;
        result.assignment.assign(impl_->variable_count + 1, -1);
        for (const auto& row : impl_->prefix_variables)
            for (const auto variable : row)
                result.assignment[variable] = impl_->solver.val(static_cast<int>(variable)) > 0;
    } else if (status == 20) {
        result.status = IncrementalStatus::unsat_exploratory;
        if (impl_->proof_open) {
            impl_->solver.conclude();
            impl_->solver.disconnect_proof_tracer(impl_->drat_tracer.get());
            impl_->proof_open = false;
            result.proof_path = "";
            result.added_unit_clauses = impl_->added_units;
            result.proof_backend = "embedded";
            result.proof_provenance = "in-memory-bytes";
            result.proof_bytes = impl_->proof_bytes;
            result.proof_hash = fnv1a64_bytes(result.proof_bytes);
        }
    } else {
        result.status = IncrementalStatus::timed_out;
    }
    return result;
#endif
}

} // namespace cutwidth::pb
