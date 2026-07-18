#pragma once

#include "graph.hpp"
#include "vertex_set.hpp"

#include <cstdint>
#include <vector>

namespace cutwidth {

// Vertices of graph are numbered densely in the same order as parent_vertices.
struct InducedComponent {
    Graph graph;
    std::vector<Graph::Vertex> parent_vertices;
    VertexSet parent_set;
};

// Returns connected components ordered by their smallest parent vertex.  Within
// each component, local vertex IDs follow increasing parent vertex IDs.
[[nodiscard]] std::vector<InducedComponent> connected_components(const Graph& graph);

enum class TwinKind : std::uint8_t {
    False, // Nonadjacent vertices with identical open neighborhoods.
    True   // Adjacent vertices with identical closed neighborhoods.
};

struct TwinClass {
    TwinKind kind;
    std::vector<Graph::Vertex> vertices;
};

// Computes global true/false-twin equivalence classes.  Singleton classes are
// omitted.  Classes and their members are returned in increasing vertex order.
// This helper identifies safe graph automorphisms; it does not itself prescribe
// a search-state pruning rule.
[[nodiscard]] std::vector<TwinClass> twin_classes(const Graph& graph);

} // namespace cutwidth
