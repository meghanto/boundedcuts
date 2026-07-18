#include "decomposition.hpp"

#include <algorithm>
#include <numeric>
#include <string>
#include <utility>

namespace cutwidth {
namespace {

using Vertex = Graph::Vertex;
InducedComponent induce(const Graph& parent, std::vector<Vertex> parent_vertices) {
    std::vector<std::pair<Vertex, Vertex>> edges;
    std::vector<std::string> labels;
    labels.reserve(parent_vertices.size());

    for (std::size_t local = 0; local < parent_vertices.size(); ++local) {
        const Vertex parent_vertex = parent_vertices[local];
        labels.push_back(parent.label(parent_vertex));
        for (std::size_t other = local + 1; other < parent_vertices.size(); ++other) {
            if (parent.adjacent(parent_vertex, parent_vertices[other])) {
                edges.emplace_back(static_cast<Vertex>(local), static_cast<Vertex>(other));
            }
        }
    }

    VertexSet parent_set(parent.size());
    for (const auto vertex : parent_vertices) parent_set.insert(vertex);
    return {Graph(parent_vertices.size(), edges, std::move(labels)),
            std::move(parent_vertices), std::move(parent_set)};
}

bool false_twins(const Graph& graph, Vertex a, Vertex b) {
    return !graph.adjacent(a, b) &&
           std::equal(graph.adjacency_words(a).begin(), graph.adjacency_words(a).end(),
                      graph.adjacency_words(b).begin());
}

bool true_twins(const Graph& graph, Vertex a, Vertex b) {
    if (!graph.adjacent(a, b)) return false;
    const auto left = graph.adjacency_words(a);
    const auto right = graph.adjacency_words(b);
    for (std::size_t word = 0; word < graph.word_count(); ++word) {
        auto lhs = left[word];
        auto rhs = right[word];
        if (word == static_cast<std::size_t>(a) / 64U) lhs |= Graph::Mask{1} << (a % 64U);
        if (word == static_cast<std::size_t>(b) / 64U) rhs |= Graph::Mask{1} << (b % 64U);
        if (lhs != rhs) return false;
    }
    return true;
}

} // namespace

std::vector<InducedComponent> connected_components(const Graph& graph) {
    std::vector<InducedComponent> result;
    std::vector<bool> seen(graph.size(), false);
    for (Vertex root = 0; root < graph.size(); ++root) {
        if (seen[root]) continue;
        std::vector<Vertex> component;
        std::vector<Vertex> frontier{root};
        seen[root] = true;
        while (!frontier.empty()) {
            const Vertex vertex = frontier.back();
            frontier.pop_back();
            component.push_back(vertex);
            for (Vertex other = 0; other < graph.size(); ++other) {
                if (!seen[other] && graph.adjacent(vertex, other)) {
                    seen[other] = true;
                    frontier.push_back(other);
                }
            }
        }
        std::sort(component.begin(), component.end());
        result.push_back(induce(graph, std::move(component)));
    }
    return result;
}

std::vector<TwinClass> twin_classes(const Graph& graph) {
    std::vector<TwinClass> result;
    std::vector<bool> assigned(graph.size(), false);
    for (const TwinKind kind : {TwinKind::False, TwinKind::True}) {
        std::fill(assigned.begin(), assigned.end(), false);
        for (Vertex representative = 0; representative < graph.size(); ++representative) {
            if (assigned[representative]) continue;
            std::vector<Vertex> vertices{representative};
            for (Vertex candidate = representative + 1; candidate < graph.size(); ++candidate) {
                if (assigned[candidate]) continue;
                const bool match = kind == TwinKind::False
                    ? false_twins(graph, representative, candidate)
                    : true_twins(graph, representative, candidate);
                if (match) {
                    assigned[candidate] = true;
                    vertices.push_back(candidate);
                }
            }
            if (vertices.size() > 1) result.push_back({kind, std::move(vertices)});
        }
    }

    // std::map orders primarily by kind, whereas callers need graph order.
    std::sort(result.begin(), result.end(), [](const TwinClass& left, const TwinClass& right) {
        return left.vertices.front() < right.vertices.front();
    });
    return result;
}

} // namespace cutwidth
