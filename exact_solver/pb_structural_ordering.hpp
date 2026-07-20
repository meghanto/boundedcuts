#pragma once

#include "graph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cutwidth::pb {

struct StructuralOrderingScore {
    std::uint32_t maximum_frontier = 0;
    std::uint32_t bandwidth = 0;
    std::uint64_t total_edge_span = 0;
};

struct StructuralOrdering {
    std::string name;
    std::vector<Graph::Vertex> permutation; // new id -> original id
    StructuralOrderingScore score;
};

[[nodiscard]] StructuralOrdering identity_structural_ordering(const Graph& graph);
[[nodiscard]] StructuralOrdering degree_bfs_structural_ordering(const Graph& graph);
[[nodiscard]] StructuralOrdering rcm_structural_ordering(const Graph& graph);
[[nodiscard]] StructuralOrdering select_structural_ordering(
    const Graph& graph, const std::string& mode);
[[nodiscard]] StructuralOrderingScore score_structural_ordering(
    const Graph& graph, const std::vector<Graph::Vertex>& permutation);
[[nodiscard]] Graph permute_graph(
    const Graph& graph, const std::vector<Graph::Vertex>& permutation);

} // namespace cutwidth::pb
