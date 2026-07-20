#include "pb_structural_ordering.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace cutwidth::pb {
namespace {

std::vector<Graph::Vertex> degree_bfs(const Graph& graph, bool reverse_components) {
    const auto n = graph.size();
    std::vector<bool> visited(n, false);
    std::vector<Graph::Vertex> result;
    result.reserve(n);

    while (result.size() != n) {
        Graph::Vertex root = 0;
        bool found = false;
        for (Graph::Vertex v = 0; v < n; ++v) {
            if (visited[v]) continue;
            if (!found || std::tuple{graph.degree(v), v} <
                              std::tuple{graph.degree(root), root}) {
                root = v;
                found = true;
            }
        }

        std::vector<Graph::Vertex> component;
        std::vector<Graph::Vertex> queue{root};
        visited[root] = true;
        for (std::size_t head = 0; head < queue.size(); ++head) {
            const auto v = queue[head];
            component.push_back(v);
            std::vector<Graph::Vertex> neighbors;
            for (Graph::Vertex u = 0; u < n; ++u)
                if (!visited[u] && graph.adjacent(v, u)) neighbors.push_back(u);
            std::sort(neighbors.begin(), neighbors.end(), [&](auto a, auto b) {
                return std::tuple{graph.degree(a), a} <
                       std::tuple{graph.degree(b), b};
            });
            for (const auto u : neighbors) {
                visited[u] = true;
                queue.push_back(u);
            }
        }
        if (reverse_components) std::reverse(component.begin(), component.end());
        result.insert(result.end(), component.begin(), component.end());
    }
    return result;
}

StructuralOrdering make_ordering(
    const Graph& graph, std::string name, std::vector<Graph::Vertex> permutation) {
    return {std::move(name), permutation,
            score_structural_ordering(graph, permutation)};
}

auto ordering_key(const StructuralOrdering& candidate) {
    return std::tuple{candidate.score.maximum_frontier,
                      candidate.score.bandwidth,
                      candidate.score.total_edge_span,
                      candidate.permutation};
}

} // namespace

StructuralOrderingScore score_structural_ordering(
    const Graph& graph, const std::vector<Graph::Vertex>& permutation) {
    if (!graph.validate_ordering(permutation))
        throw std::invalid_argument("structural ordering is not a permutation");
    const auto n = graph.size();
    std::vector<std::size_t> position(n);
    for (std::size_t i = 0; i < n; ++i) position[permutation[i]] = i;

    StructuralOrderingScore score;
    std::vector<std::uint32_t> remove_at(n, 0);
    std::uint32_t frontier = 0;
    for (std::size_t i = 0; i < n; ++i) {
        frontier -= remove_at[i];
        const auto v = permutation[i];
        std::size_t last_neighbor = i;
        for (Graph::Vertex u = 0; u < n; ++u) {
            if (!graph.adjacent(v, u)) continue;
            const auto span = position[v] > position[u]
                ? position[v] - position[u] : position[u] - position[v];
            score.bandwidth = std::max(score.bandwidth,
                static_cast<std::uint32_t>(span));
            if (v < u) score.total_edge_span += span;
            last_neighbor = std::max(last_neighbor, position[u]);
        }
        if (last_neighbor > i) {
            ++frontier;
            ++remove_at[last_neighbor];
        }
        score.maximum_frontier = std::max(score.maximum_frontier, frontier);
    }
    return score;
}

StructuralOrdering identity_structural_ordering(const Graph& graph) {
    std::vector<Graph::Vertex> permutation(graph.size());
    std::iota(permutation.begin(), permutation.end(), Graph::Vertex{0});
    return make_ordering(graph, "identity", std::move(permutation));
}

StructuralOrdering degree_bfs_structural_ordering(const Graph& graph) {
    return make_ordering(graph, "degree-bfs", degree_bfs(graph, false));
}

StructuralOrdering rcm_structural_ordering(const Graph& graph) {
    return make_ordering(graph, "rcm", degree_bfs(graph, true));
}

StructuralOrdering select_structural_ordering(
    const Graph& graph, const std::string& mode) {
    if (mode == "identity") return identity_structural_ordering(graph);
    if (mode == "rcm") return rcm_structural_ordering(graph);
    if (mode != "auto")
        throw std::invalid_argument("PB root ordering must be auto, identity, or rcm");
    auto best = identity_structural_ordering(graph);
    for (auto candidate : {degree_bfs_structural_ordering(graph),
                           rcm_structural_ordering(graph)})
        if (ordering_key(candidate) < ordering_key(best)) best = std::move(candidate);
    return best;
}

Graph permute_graph(
    const Graph& graph, const std::vector<Graph::Vertex>& permutation) {
    if (!graph.validate_ordering(permutation))
        throw std::invalid_argument("structural ordering is not a permutation");
    std::vector<Graph::Vertex> inverse(graph.size());
    std::vector<std::string> labels;
    labels.reserve(graph.size());
    for (std::size_t i = 0; i < graph.size(); ++i) {
        inverse[permutation[i]] = static_cast<Graph::Vertex>(i);
        labels.push_back(graph.label(permutation[i]));
    }
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    edges.reserve(graph.edge_count());
    for (Graph::Vertex u = 0; u < graph.size(); ++u)
        for (Graph::Vertex v = u + 1; v < graph.size(); ++v)
            if (graph.adjacent(u, v)) edges.emplace_back(inverse[u], inverse[v]);
    return Graph(graph.size(), edges, std::move(labels));
}

} // namespace cutwidth::pb
