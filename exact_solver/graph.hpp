#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cutwidth {

class Graph {
public:
    using Vertex = std::uint32_t;
    using Mask = std::uint64_t;

    Graph(std::size_t vertex_count,
          const std::vector<std::pair<Vertex, Vertex>>& edges,
          std::vector<std::string> labels = {});

    [[nodiscard]] static Graph from_interleaved_edges(
        std::size_t vertex_count, std::span<const Vertex> edges,
        std::vector<std::string> labels = {});

    static Graph read_edge_list(std::istream& input);

    [[nodiscard]] std::size_t size() const noexcept { return vertex_count_; }
    [[nodiscard]] std::size_t edge_count() const noexcept { return edge_count_; }
    [[nodiscard]] std::size_t word_count() const noexcept { return word_count_; }
    [[nodiscard]] bool supports_mask() const noexcept { return size() <= 63; }
    // The single-word API is retained for the original fast engine. Calling it
    // for a dynamic graph is an explicit error instead of silently truncating.
    [[nodiscard]] Mask adjacency(Vertex vertex) const;
    [[nodiscard]] std::span<const Mask> adjacency_words(Vertex vertex) const;
    [[nodiscard]] bool adjacent(Vertex a, Vertex b) const;
    [[nodiscard]] std::uint32_t degree(Vertex vertex) const;
    [[nodiscard]] const std::string& label(Vertex vertex) const;
    [[nodiscard]] std::uint32_t cut(Mask prefix) const;
    [[nodiscard]] std::uint32_t cut(std::span<const Mask> prefix) const;
    [[nodiscard]] bool validate_ordering(const std::vector<Vertex>& ordering) const;
    [[nodiscard]] std::uint32_t ordering_cutwidth(const std::vector<Vertex>& ordering) const;

private:
    struct InterleavedEdgesTag {};
    Graph(InterleavedEdgesTag, std::size_t vertex_count,
          std::span<const Vertex> edges, std::vector<std::string> labels);
    void initialize(std::size_t vertex_count, std::vector<std::string> labels);
    void add_edge(Vertex a, Vertex b);

    std::size_t vertex_count_ = 0;
    std::size_t word_count_ = 0;
    // Row-major: word_count_ consecutive words per vertex.
    std::vector<Mask> adjacency_;
    std::vector<std::string> labels_;
    std::size_t edge_count_ = 0;
};

} // namespace cutwidth
