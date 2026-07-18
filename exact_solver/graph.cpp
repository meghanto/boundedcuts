#include "graph.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace cutwidth {
namespace {

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool unsigned_integer(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::size_t as_size(const std::string& value) {
    std::size_t used = 0;
    const auto result = std::stoull(value, &used);
    if (used != value.size() || result > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("invalid integer in edge list: " + value);
    return static_cast<std::size_t>(result);
}

} // namespace

void Graph::initialize(std::size_t vertex_count, std::vector<std::string> labels) {
    vertex_count_ = vertex_count;
    word_count_ = (vertex_count + 63U) / 64U;
    edge_count_ = 0;
    labels_ = std::move(labels);
    if (vertex_count > std::numeric_limits<Vertex>::max())
        throw std::invalid_argument("vertex count exceeds the supported identifier range");
    if (word_count_ != 0 && vertex_count > std::numeric_limits<std::size_t>::max() / word_count_)
        throw std::length_error("adjacency storage size overflows");
    adjacency_.assign(vertex_count * word_count_, 0);
    if (labels_.empty()) {
        labels_.reserve(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) labels_.push_back(std::to_string(i));
    }
    if (labels_.size() != vertex_count) throw std::invalid_argument("label count differs from vertex count");
}

void Graph::add_edge(Vertex a, Vertex b) {
    if (a >= vertex_count_ || b >= vertex_count_)
        throw std::invalid_argument("edge endpoint out of range");
    if (a == b) return; // Loops never cross a cut.
    const std::size_t a_word = static_cast<std::size_t>(a) / 64U;
    const std::size_t b_word = static_cast<std::size_t>(b) / 64U;
    const Mask bit_a = Mask{1} << (a % 64U);
    const Mask bit_b = Mask{1} << (b % 64U);
    Mask& a_to_b = adjacency_[static_cast<std::size_t>(a) * word_count_ + b_word];
    if ((a_to_b & bit_b) != 0) return; // Normalize parallel edges.
    a_to_b |= bit_b;
    adjacency_[static_cast<std::size_t>(b) * word_count_ + a_word] |= bit_a;
    ++edge_count_;
}

Graph::Graph(std::size_t vertex_count,
             const std::vector<std::pair<Vertex, Vertex>>& edges,
             std::vector<std::string> labels) {
    initialize(vertex_count, std::move(labels));
    for (const auto [a, b] : edges) add_edge(a, b);
}

Graph::Graph(InterleavedEdgesTag, std::size_t vertex_count,
             std::span<const Vertex> edges, std::vector<std::string> labels) {
    if (edges.size() % 2 != 0)
        throw std::invalid_argument("interleaved edge array must have even length");
    initialize(vertex_count, std::move(labels));
    for (std::size_t i = 0; i < edges.size(); i += 2)
        add_edge(edges[i], edges[i + 1]);
}

Graph Graph::from_interleaved_edges(
    std::size_t vertex_count, std::span<const Vertex> edges,
    std::vector<std::string> labels) {
    return Graph(InterleavedEdgesTag{}, vertex_count, edges, std::move(labels));
}

Graph Graph::read_edge_list(std::istream& input) {
    std::vector<std::pair<std::string, std::string>> raw;
    std::vector<std::string> singleton_labels;
    bool named_format = false;
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        line = trim(std::move(line));
        if (line.empty()) continue;
        if (line.rfind("name:", 0) == 0) { named_format = true; continue; }
        std::istringstream words(line);
        std::string a, b, extra;
        words >> a;
        if (!(words >> b)) {
            singleton_labels.push_back(std::move(a));
            continue;
        }
        if (words >> extra)
            throw std::runtime_error("expected one or two fields per edge-list line: " + line);
        raw.emplace_back(std::move(a), std::move(b));
    }
    if (raw.empty() && singleton_labels.empty()) return Graph(0, {});
    if (raw.empty()) {
        std::vector<std::string> labels;
        std::unordered_map<std::string, Vertex> seen;
        for (auto& name : singleton_labels) {
            if (seen.emplace(name, static_cast<Vertex>(labels.size())).second)
                labels.push_back(std::move(name));
        }
        const auto vertex_count = labels.size();
        return Graph(vertex_count, {}, std::move(labels));
    }

    std::size_t declared_n = 0;
    std::size_t begin = 0;
    const bool possible_header = unsigned_integer(raw[0].first) && unsigned_integer(raw[0].second);
    if (possible_header) {
        const auto n = as_size(raw[0].first);
        const auto m = as_size(raw[0].second);
        bool endpoints_fit = n > 0;
        bool zero_based = true, one_based = true;
        for (std::size_t i = 1; i < raw.size() && endpoints_fit; ++i) {
            if (!unsigned_integer(raw[i].first) || !unsigned_integer(raw[i].second)) {
                endpoints_fit = false;
                break;
            }
            for (const auto* value : {&raw[i].first, &raw[i].second}) {
                const auto endpoint = as_size(*value);
                zero_based &= endpoint < n;
                one_based &= endpoint >= 1 && endpoint <= n;
            }
        }
        endpoints_fit &= zero_based || one_based;
        // A bare numeric edge can be indistinguishable from an `n m` header.
        // Only the paper's explicit `name:` marker enables header parsing.
        if (named_format) {
            if (n > std::numeric_limits<Vertex>::max())
                throw std::runtime_error("declared vertex count exceeds the supported identifier range");
            if (m != raw.size() - 1) throw std::runtime_error("declared edge count does not match file");
            if (!endpoints_fit && m != 0)
                throw std::runtime_error("edge endpoint outside declared vertex range");
            declared_n = n;
            begin = 1;
        }
    }

    std::vector<std::pair<std::string, std::string>> edges(raw.begin() + static_cast<std::ptrdiff_t>(begin), raw.end());
    std::vector<std::string> labels;
    std::unordered_map<std::string, Vertex> ids;

    if (declared_n != 0) {
        bool one_based = true, zero_based = true;
        for (const auto& [a, b] : edges) {
            if (!unsigned_integer(a) || !unsigned_integer(b)) { one_based = zero_based = false; break; }
            for (const auto* s : {&a, &b}) {
                const auto x = as_size(*s);
                one_based &= x >= 1 && x <= declared_n;
                zero_based &= x < declared_n;
            }
        }
        // Prefer the one-based convention used by the bundled instances.
        if (one_based || zero_based) {
            labels.reserve(declared_n);
            for (std::size_t i = 0; i < declared_n; ++i) {
                labels.push_back(std::to_string(one_based ? i + 1 : i));
                ids.emplace(labels.back(), static_cast<Vertex>(i));
            }
        }
    }

    auto id_for = [&](const std::string& name) -> Vertex {
        if (const auto it = ids.find(name); it != ids.end()) return it->second;
        if (declared_n != 0 && labels.size() >= declared_n)
            throw std::runtime_error("edge endpoint outside declared vertex range: " + name);
        const auto id = static_cast<Vertex>(labels.size());
        ids.emplace(name, id);
        labels.push_back(name);
        return id;
    };

    std::vector<std::pair<Vertex, Vertex>> normalized;
    normalized.reserve(edges.size());
    for (const auto& name : singleton_labels) (void)id_for(name);
    for (const auto& [a, b] : edges) normalized.emplace_back(id_for(a), id_for(b));
    if (declared_n != 0 && labels.size() < declared_n) {
        // Non-numeric labels cannot identify unnamed isolated vertices.
        throw std::runtime_error("declared isolated vertices require numeric zero- or one-based labels");
    }
    const auto vertex_count = declared_n != 0 ? declared_n : labels.size();
    return Graph(vertex_count, normalized, std::move(labels));
}

Graph::Mask Graph::adjacency(Vertex vertex) const {
    if (vertex >= size()) throw std::out_of_range("vertex out of range");
    if (!supports_mask()) throw std::logic_error("single-word adjacency requested for a dynamic graph");
    return adjacency_[static_cast<std::size_t>(vertex) * word_count_];
}

std::span<const Graph::Mask> Graph::adjacency_words(Vertex vertex) const {
    if (vertex >= size()) throw std::out_of_range("vertex out of range");
    const auto offset = static_cast<std::size_t>(vertex) * word_count_;
    return {adjacency_.data() + offset, word_count_};
}

bool Graph::adjacent(Vertex a, Vertex b) const {
    if (a >= size() || b >= size()) throw std::out_of_range("vertex out of range");
    const auto row = adjacency_words(a);
    return (row[static_cast<std::size_t>(b) / 64U] & (Mask{1} << (b % 64U))) != 0;
}

std::uint32_t Graph::degree(Vertex vertex) const {
    std::uint32_t result = 0;
    for (const Mask word : adjacency_words(vertex))
        result += static_cast<std::uint32_t>(std::popcount(word));
    return result;
}

const std::string& Graph::label(Vertex vertex) const {
    if (vertex >= size()) throw std::out_of_range("vertex out of range");
    return labels_[vertex];
}

std::uint32_t Graph::cut(Mask prefix) const {
    if (!supports_mask()) throw std::logic_error("single-word cut requested for a dynamic graph");
    std::uint32_t result = 0;
    Mask vertices = prefix;
    while (vertices) {
        const auto v = static_cast<Vertex>(std::countr_zero(vertices));
        vertices &= vertices - 1;
        result += static_cast<std::uint32_t>(std::popcount(adjacency_[v * word_count_] & ~prefix));
    }
    return result;
}

std::uint32_t Graph::cut(std::span<const Mask> prefix) const {
    if (prefix.size() != word_count_) throw std::invalid_argument("prefix word count differs from graph");
    if (supports_mask()) return cut(prefix.empty() ? 0 : prefix.front());
    std::uint64_t result = 0;
    for (std::size_t vertex = 0; vertex < size(); ++vertex) {
        if ((prefix[vertex / 64U] & (Mask{1} << (vertex % 64U))) == 0) continue;
        const auto neighbors = adjacency_words(static_cast<Vertex>(vertex));
        for (std::size_t word = 0; word < word_count_; ++word)
            result += static_cast<std::uint64_t>(std::popcount(neighbors[word] & ~prefix[word]));
    }
    if (result > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("cut size exceeds 32-bit result range");
    return static_cast<std::uint32_t>(result);
}

bool Graph::validate_ordering(const std::vector<Vertex>& ordering) const {
    if (ordering.size() != size()) return false;
    std::vector<Mask> seen(word_count_, 0);
    for (const Vertex v : ordering) {
        if (v >= size()) return false;
        const Mask bit = Mask{1} << (v % 64U);
        Mask& word = seen[static_cast<std::size_t>(v) / 64U];
        if ((word & bit) != 0) return false;
        word |= bit;
    }
    return true;
}

std::uint32_t Graph::ordering_cutwidth(const std::vector<Vertex>& ordering) const {
    if (!validate_ordering(ordering)) throw std::invalid_argument("invalid vertex ordering");
    std::vector<Mask> prefix(word_count_, 0);
    std::uint32_t width = 0;
    for (const Vertex v : ordering) {
        prefix[static_cast<std::size_t>(v) / 64U] |= Mask{1} << (v % 64U);
        width = std::max(width, cut(prefix));
    }
    return width;
}

} // namespace cutwidth
