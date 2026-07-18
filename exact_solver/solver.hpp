#pragma once

#include "graph.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cutwidth {

struct SolverOptions {
    // Zero means no time limit.
    std::chrono::milliseconds time_limit{0};
    // Zero means unlimited. Once full, the table remains useful but receives no new states.
    std::size_t transposition_table_limit = 0;
    bool use_transposition_table = true;
};

struct SolverResult {
    bool optimal = false;
    std::uint32_t lower_bound = 0;
    std::uint32_t upper_bound = 0;
    std::vector<Graph::Vertex> ordering;
    std::uint64_t nodes_expanded = 0;
    std::uint64_t pruned_by_bound = 0;
    std::uint64_t pruned_by_dominance = 0;
    std::size_t transposition_table_size = 0;
};

class ExactSolver {
public:
    explicit ExactSolver(const Graph& graph, SolverOptions options = {});
    [[nodiscard]] SolverResult solve();

private:
    const Graph& graph_;
    SolverOptions options_;
};

} // namespace cutwidth
