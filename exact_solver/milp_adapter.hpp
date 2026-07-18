#pragma once

#include "graph.hpp"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutwidth {

struct MilpModelOptions {
    // When set, add W <= threshold and use the model as an exact decision test.
    std::optional<std::uint32_t> threshold;
    // Keep the orientation with vertex 0 before vertex n-1. Reversal preserves
    // every prefix-cut multiset, so this removes exactly one of two orientations.
    bool reversal_symmetry = true;
    Graph::Vertex reversal_first_vertex = 0;
    // Omitted means the last dense internal vertex ID. These are generic
    // symmetry anchors, never selected from instance identity or known results.
    std::optional<Graph::Vertex> reversal_second_vertex;
};

// Writes a solver-independent CPLEX-LP model of cutwidth. Only assignment
// variables are integral; prefix and edge-crossing variables are implied by
// them and remain continuous.
void write_cutwidth_lp(std::ostream& output, const Graph& graph,
                       MilpModelOptions options = {});

enum class MilpStatus { optimal, infeasible, limit, unavailable, error, unknown };

struct MilpResult {
    MilpStatus status = MilpStatus::unknown;
    std::optional<std::uint32_t> optimum;
    std::optional<std::uint32_t> incumbent_width;
    int exit_code = -1;
    std::string diagnostic;
    std::vector<Graph::Vertex> ordering;
    double model_build_seconds = 0.0;
    double solve_seconds = 0.0;
    std::int64_t mip_nodes = 0;
    std::optional<double> diagnostic_dual_bound;
};

// Strictly parses HiGHS' summary. Only an explicit Optimal status with an
// integral primal bound, or explicit Infeasible status, is certifying.
[[nodiscard]] MilpResult parse_highs_output(std::string_view output,
                                            int exit_code = 0);

// Optional in-process HiGHS C++ API adapter. When the library was not found at
// configure time this returns `unavailable`; the exact core remains standalone.
[[nodiscard]] MilpResult run_highs(const Graph& graph,
                                   MilpModelOptions model = {},
                                   std::optional<double> time_limit_seconds = std::nullopt);

} // namespace cutwidth
