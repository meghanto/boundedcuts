#include "graph.hpp"
#include "partial_bounds.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using cutwidth::Graph;

Graph graph_from_bits(std::uint32_t n, std::uint64_t bits) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    std::uint32_t edge = 0;
    for (std::uint32_t u = 0; u < n; ++u)
        for (std::uint32_t v = u + 1; v < n; ++v, ++edge)
            if ((bits >> edge) & 1U) edges.emplace_back(u, v);
    return Graph(n, edges);
}

std::uint32_t exact_completion(const Graph& graph, Graph::Mask prefix) {
    std::vector<Graph::Vertex> remaining;
    for (Graph::Vertex v = 0; v < graph.size(); ++v)
        if ((prefix & (Graph::Mask{1} << v)) == 0) remaining.push_back(v);
    std::uint32_t optimum = UINT32_MAX;
    do {
        auto state = prefix;
        auto width = graph.cut(prefix);
        for (const auto v : remaining) {
            state |= Graph::Mask{1} << v;
            width = std::max(width, graph.cut(state));
        }
        optimum = std::min(optimum, width);
    } while (std::next_permutation(remaining.begin(), remaining.end()));
    return optimum == UINT32_MAX ? graph.cut(prefix) : optimum;
}

std::uint32_t exact_cutwidth_dp(const Graph& graph) {
    const Graph::Mask count = Graph::Mask{1} << graph.size();
    std::vector<std::uint32_t> dp(count, UINT32_MAX);
    dp[0] = 0;
    for (Graph::Mask subset = 1; subset < count; ++subset) {
        const auto cut = graph.cut(subset);
        auto vertices = subset;
        while (vertices) {
            const auto bit = vertices & (~vertices + 1);
            vertices -= bit;
            dp[subset] = std::min(dp[subset], std::max(dp[subset ^ bit], cut));
        }
    }
    return dp.back();
}

void require_valid(const char* name, std::uint32_t value, std::uint32_t optimum,
                   std::uint32_t n, std::uint64_t graph_bits, Graph::Mask prefix) {
    if (value <= optimum) return;
    throw std::runtime_error(std::string(name) + " invalid: n=" + std::to_string(n) +
        " graph=" + std::to_string(graph_bits) + " prefix=" + std::to_string(prefix) +
        " bound=" + std::to_string(value) + " optimum=" + std::to_string(optimum));
}

void exhaustive_bound_validation() {
    for (std::uint32_t n = 0; n <= 5; ++n) {
        const auto pairs = n * (n - 1) / 2;
        const std::uint64_t graph_count = std::uint64_t{1} << pairs;
        const Graph::Mask subset_count = Graph::Mask{1} << n;
        for (std::uint64_t bits = 0; bits < graph_count; ++bits) {
            const auto graph = graph_from_bits(n, bits);
            const auto global_optimum = exact_completion(graph, 0);
            require_valid("average degree",
                cutwidth::average_degree_lower_bound(graph.size(), graph.edge_count()),
                global_optimum, n, bits, 0);
            require_valid("grooming density",
                cutwidth::grooming_density_lower_bound(graph.size(), graph.edge_count()),
                global_optimum, n, bits, 0);
            cutwidth::PartialBoundOptions all_bounds{true, true, true, true};
            cutwidth::PartialBoundEvaluator evaluator(graph, all_bounds);
            for (Graph::Mask prefix = 0; prefix < subset_count; ++prefix) {
                const auto optimum = exact_completion(graph, prefix);
                const auto values = evaluator.evaluate(prefix,
                    static_cast<std::uint32_t>(std::popcount(prefix)));
                require_valid("current", values.current_cut, optimum, n, bits, prefix);
                require_valid("residual degree", values.residual_degree, optimum, n, bits, prefix);
                require_valid("edge-distance area", values.edge_distance_area, optimum, n, bits, prefix);
                require_valid("degree-distance area", values.degree_distance_area, optimum, n, bits, prefix);
                require_valid("degeneracy", values.degeneracy, optimum, n, bits, prefix);
                require_valid("combined", values.combined, optimum, n, bits, prefix);

                cutwidth::PartialBoundStats stats;
                if (optimum > 0 && evaluator.exceeds(prefix,
                        static_cast<std::uint32_t>(std::popcount(prefix)), optimum - 1, stats)) {
                    // A rejection below the exact completion optimum is sound.
                }
                if (evaluator.exceeds(prefix,
                        static_cast<std::uint32_t>(std::popcount(prefix)), optimum, stats))
                    throw std::runtime_error("evaluator rejected its exact completion optimum");
            }
        }
    }
}

void grooming_source_regressions() {
    for (std::size_t n = 2; n <= 30; ++n) {
        if (cutwidth::grooming_request_capacity(n, 1) != n - 1)
            throw std::runtime_error("T(1,n) source regression failed");
        if (cutwidth::grooming_request_capacity(n, 2) != (3 * n - 3) / 2)
            throw std::runtime_error("T(2,n) source regression failed");
        if (n >= 3 && cutwidth::grooming_request_capacity(n, 3) != 2 * n - 3)
            throw std::runtime_error("T(3,n) source regression failed");
        if (n >= 6 && cutwidth::grooming_request_capacity(n, 6) != 3 * n - 6)
            throw std::runtime_error("T(6,n) source regression failed");
    }
    if (cutwidth::grooming_request_capacity(11, 10) != 35)
        throw std::runtime_error("T(10,11) source anomaly regression failed");
    if (cutwidth::grooming_request_capacity(16, 21) != 77)
        throw std::runtime_error("T(21,16) source anomaly regression failed");
}

void average_degree_integer_arithmetic_tests() {
    for (std::uint64_t n = 2; n <= 200; ++n) {
        const auto maximum_edges = n * (n - 1) / 2;
        for (std::uint64_t m = 0; m <= maximum_edges; ++m) {
            const long double d = static_cast<long double>(n - 1);
            const auto reference = static_cast<std::uint32_t>(std::ceil(
                static_cast<long double>(m) * (m + d) / (2 * d * d)));
            if (cutwidth::average_degree_lower_bound(n, m) != reference)
                throw std::runtime_error("average-degree integer arithmetic mismatch");
        }
    }
}

void exhaustive_six_vertex_grooming_validation() {
    constexpr std::uint32_t n = 6;
    constexpr std::uint32_t pairs = n * (n - 1) / 2;
    for (std::uint64_t bits = 0; bits < (std::uint64_t{1} << pairs); ++bits) {
        const auto graph = graph_from_bits(n, bits);
        require_valid("six-vertex grooming density",
            cutwidth::grooming_density_lower_bound(n, graph.edge_count()),
            exact_cutwidth_dp(graph), n, bits, 0);
    }
}

void session_ceiling_gates_hot_path() {
    const Graph path(8, {{0,1},{1,2},{2,3},{3,4},{4,5},{5,6},{6,7}});
    cutwidth::PartialBoundOptions options;
    options.residual_degree = true;
    options.degeneracy = true;
    const auto residual_ceiling =
        cutwidth::PartialBoundEvaluator::residual_degree_session_ceiling(path);
    const auto degeneracy_ceiling =
        cutwidth::PartialBoundEvaluator::degeneracy_session_ceiling(path);
    if (residual_ceiling != 1 || degeneracy_ceiling != 1)
        throw std::runtime_error("session ceiling formula regression");
    cutwidth::PartialBoundEvaluator evaluator(path, options, 1);
    cutwidth::PartialBoundStats stats;
    evaluator.note_session_ceiling_skips(stats);
    if (evaluator.exceeds(Graph::Mask{0}, 0, 1, stats) ||
        stats.evaluations != 0 || stats.residual_degree_evaluations != 0 ||
        stats.degeneracy_evaluations != 0 ||
        stats.residual_degree_session_ceiling_skips != 1 ||
        stats.degeneracy_session_ceiling_skips != 1)
        throw std::runtime_error("inapplicable session bound entered the hot path");
}

void expensive_bounds_respect_slack_gate() {
    const Graph path(6, {{0,1},{1,2},{2,3},{3,4},{4,5}});
    cutwidth::PartialBoundOptions options;
    options.residual_degree = false;
    options.edge_distance_area = true;
    options.expensive_max_slack = 0;
    cutwidth::PartialBoundEvaluator gated(path, options, 3);
    cutwidth::PartialBoundStats gated_stats;
    const Graph::Mask prefix = Graph::Mask{1};
    if (gated.completion_exceeds(
            std::span<const Graph::Mask>(&prefix, 1), 1, 3, gated_stats,
            std::nullopt, 1) ||
        gated_stats.expensive_slack_gate_skips != 1 ||
        gated_stats.edge_distance_area_evaluations != 0)
        throw std::runtime_error("expensive partial bound ignored its slack gate");

    options.expensive_max_slack = 2;
    cutwidth::PartialBoundEvaluator admitted(path, options, 3);
    cutwidth::PartialBoundStats admitted_stats;
    (void)admitted.completion_exceeds(
        std::span<const Graph::Mask>(&prefix, 1), 1, 3, admitted_stats,
        std::nullopt, 1);
    if (admitted_stats.expensive_slack_gate_skips != 0 ||
        admitted_stats.edge_distance_area_evaluations != 1)
        throw std::runtime_error("tight partial bound was not evaluated");
}

void triangle_free_degeneracy_specialization() {
    const Graph complete_bipartite(6, {
        {0,3},{0,4},{0,5},{1,3},{1,4},{1,5},{2,3},{2,4},{2,5}});
    const Graph clique(4, {{0,1},{0,2},{0,3},{1,2},{1,3},{2,3}});
    if (cutwidth::PartialBoundEvaluator::degeneracy_session_ceiling(
            complete_bipartite) != 5 ||
        cutwidth::PartialBoundEvaluator::degeneracy_session_ceiling(clique) != 4)
        throw std::runtime_error("Kloeckner triangle-free specialization regression");
}
} // namespace

int main() {
    try {
        exhaustive_bound_validation();
        grooming_source_regressions();
        average_degree_integer_arithmetic_tests();
        exhaustive_six_vertex_grooming_validation();
        session_ceiling_gates_hot_path();
        expensive_bounds_respect_slack_gate();
        triangle_free_degeneracy_specialization();
        std::cout << "All partial-bound exhaustive tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BOUND TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
