#include "graph.hpp"
#include "lagrangian_bound.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cutwidth::Graph;
using cutwidth::LagrangianPrefixBoundEvaluator;
using cutwidth::LagrangianTelemetry;

// Reusable ceil division definition for verification
int64_t verified_ceil_div(int64_t x, int64_t y) {
    int64_t q = x / y;
    int64_t r = x % y;
    if (r > 0) {
        return q + 1;
    }
    return q;
}

void test_signed_ceil_division() {
    // Test positive, negative, and exact division cases
    if (verified_ceil_div(5, 2) != 3) throw std::runtime_error("Ceil div error: 5/2");
    if (verified_ceil_div(4, 2) != 2) throw std::runtime_error("Ceil div error: 4/2");
    if (verified_ceil_div(0, 3) != 0) throw std::runtime_error("Ceil div error: 0/3");
    if (verified_ceil_div(-1, 3) != 0) throw std::runtime_error("Ceil div error: -1/3");
    if (verified_ceil_div(-3, 3) != -1) throw std::runtime_error("Ceil div error: -3/3");
    if (verified_ceil_div(-4, 3) != -1) throw std::runtime_error("Ceil div error: -4/3");
    if (verified_ceil_div(-5, 2) != -2) throw std::runtime_error("Ceil div error: -5/2");
    if (verified_ceil_div(-4, 2) != -2) throw std::runtime_error("Ceil div error: -4/2");
}

Graph graph_from_bits(std::uint32_t n, std::uint64_t bits) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    std::uint32_t edge = 0;
    for (std::uint32_t u = 0; u < n; ++u) {
        for (std::uint32_t v = u + 1; v < n; ++v, ++edge) {
            if ((bits >> edge) & 1U) {
                edges.emplace_back(u, v);
            }
        }
    }
    return Graph(n, edges);
}

// Compute q_t(S) exactly by subset enumeration
std::uint32_t exact_q_t(const Graph& graph, Graph::Mask prefix, std::uint32_t t) {
    std::vector<Graph::Vertex> U;
    for (Graph::Vertex v = 0; v < graph.size(); ++v) {
        if ((prefix & (Graph::Mask{1} << v)) == 0) {
            U.push_back(v);
        }
    }
    if (t == 0 || t >= U.size()) {
        throw std::invalid_argument("invalid t for exact_q_t");
    }

    std::uint32_t min_cut = UINT32_MAX;

    // Generate all subsets T of U of size t
    std::vector<bool> sel(U.size(), false);
    std::fill(sel.end() - t, sel.end(), true);

    do {
        Graph::Mask union_prefix = prefix;
        for (std::size_t i = 0; i < U.size(); ++i) {
            if (sel[i]) {
                union_prefix |= (Graph::Mask{1} << U[i]);
            }
        }
        min_cut = std::min(min_cut, graph.cut(union_prefix));
    } while (std::next_permutation(sel.begin(), sel.end()));

    return min_cut;
}

void test_exhaustive_graphs() {
    // Generate all graphs up to n=5.
    // For each graph, for all prefixes, for all valid t:
    // Compute exact q_t(S) and assert Lagrangian lower bound <= exact q_t(S).
    for (std::uint32_t n = 2; n <= 5; ++n) {
        const std::uint32_t pairs = n * (n - 1) / 2;
        const std::uint64_t graph_count = std::uint64_t{1} << pairs;
        const Graph::Mask prefix_count = Graph::Mask{1} << n;

        for (std::uint64_t bits = 0; bits < graph_count; ++bits) {
            const auto graph = graph_from_bits(n, bits);
            LagrangianPrefixBoundEvaluator evaluator(graph);

            for (Graph::Mask prefix = 0; prefix < prefix_count; ++prefix) {
                // Determine U size
                std::uint32_t u_count = n - std::popcount(prefix);
                if (u_count <= 1) {
                    // Should be ineligible
                    auto tel = evaluator.evaluate(prefix);
                    if (!tel.ineligible) {
                        throw std::runtime_error("expected ineligible for prefix with |U| <= 1");
                    }
                    continue;
                }

                // Check each valid cardinality t
                for (std::uint32_t t = 1; t < u_count; ++t) {
                    const std::uint32_t exact_bound = exact_q_t(graph, prefix, t);

                    // Evaluate with selected cardinality
                    auto tel = evaluator.evaluate(prefix, std::nullopt, {t});

                    if (tel.overflow) {
                        // Overflow should be safe
                        continue;
                    }
                    if (tel.certified_bound > exact_bound) {
                        throw std::runtime_error("Lagrangian bound invalid: " +
                            std::to_string(tel.certified_bound) + " > exact " +
                            std::to_string(exact_bound) + " for n=" + std::to_string(n) +
                            ", bits=" + std::to_string(bits) +
                            ", prefix=" + std::to_string(prefix) +
                            ", t=" + std::to_string(t));
                    }
                }

                // A completion passes through every residual cardinality, so
                // max_t q_t is a lower bound on its maximum future cut.
                auto tel_global = evaluator.evaluate(prefix);
                if (!tel_global.overflow && !tel_global.ineligible) {
                    std::uint32_t max_exact = 0;
                    for (std::uint32_t t = 1; t < u_count; ++t) {
                        max_exact = std::max(max_exact, exact_q_t(graph, prefix, t));
                    }
                    if (tel_global.certified_bound > max_exact) {
                        throw std::runtime_error("Lagrangian global bound invalid: " +
                            std::to_string(tel_global.certified_bound) + " > max_exact " +
                            std::to_string(max_exact));
                    }
                }
            }
        }
    }
}

void test_empty_full_exclusions() {
    // Graph of 4 vertices: path 0-1-2-3
    Graph graph(4, {{0, 1}, {1, 2}, {2, 3}});
    LagrangianPrefixBoundEvaluator evaluator(graph);

    // Empty prefix S (all vertices in U)
    auto tel_empty = evaluator.evaluate(Graph::Mask{0});
    if (tel_empty.ineligible) {
        throw std::runtime_error("Empty prefix S should not be ineligible");
    }
    // S union T when |T| = 2 has cut at least 1 (e.g. {0,1} cut is 1, {1,2} cut is 2, etc.)
    // Verify bound is sound
    if (tel_empty.certified_bound > 1) {
        throw std::runtime_error("Invalid bound for empty prefix on path-4");
    }

    // Full prefix S = {0,1,2,3} (U is empty, size 0 <= 1) -> must be ineligible
    auto tel_full = evaluator.evaluate(Graph::Mask{15});
    if (!tel_full.ineligible) {
        throw std::runtime_error("Full prefix S should be ineligible");
    }

    // Exclusion case where |U| = 1: S = {0,1,2} (U = {3}) -> must be ineligible
    auto tel_one_left = evaluator.evaluate(Graph::Mask{7});
    if (!tel_one_left.ineligible) {
        throw std::runtime_error("Prefix with |U|=1 should be ineligible");
    }
}

void test_nonuniform_boundary_terms_and_negative_unary() {
    // 5-vertex graph:
    // S = {0, 1}
    // U = {2, 3, 4}
    // Edges between S and U:
    // 0-2 (1 edge), 1-2 (1 edge) => a_2 = 2
    // 0-3 (1 edge) => a_3 = 1
    // No edges between S and 4 => a_4 = 0
    // Edges within U:
    // 2-3, 3-4
    Graph graph(5, {
        {0, 1},
        {0, 2}, {1, 2},
        {0, 3},
        {2, 3}, {3, 4}
    });

    LagrangianPrefixBoundEvaluator evaluator(graph);
    // S = {0, 1} => Mask = 3
    auto tel = evaluator.evaluate(3U);
    if (tel.ineligible || tel.overflow) {
        throw std::runtime_error("Failed nonuniform boundary test evaluation");
    }

    // For S={0,1}, exact q_t for t=1:
    // subsets T of U with size 1:
    // T = {2}: S union T = {0,1,2}, cut is 0-3 (1 edge), 1-2 is inside, 0-2 is inside. Wait:
    // S union T = {0,1,2}. Boundary edges are:
    // 0-3 (crosses), 2-3 (crosses). Total cut size = 2.
    // T = {3}: S union T = {0,1,3}, cut is 0-2 (crosses), 1-2 (crosses), 3-2 (crosses), 3-4 (crosses). Total cut size = 4.
    // T = {4}: S union T = {0,1,4}, cut is 0-2, 1-2, 0-3, 4-3. Total cut size = 4.
    // So exact q_1 = min(2, 4, 4) = 2.
    // Lagrangian certified bound must be <= 2.
    if (tel.certified_bound > 2) {
        throw std::runtime_error("Lagrangian bound exceeds exact q_t on nonuniform boundary case");
    }

    // Let's test with negative unary coefficients w_v = p - q * a_v.
    // If we specify lambda = p/q = 1/2, then for v=2, a_2 = 2.
    // w_2 = p - q * a_2 = 1 - 2*2 = -3 < 0.
    // This verifies the negative unary coefficient code path (w_2 < 0).
    // Let's run a targeted test for denominator 2 and numerator 1.
    auto tel_neg = evaluator.evaluate(3U, std::nullopt, {}, 2U, std::vector<int64_t>{1});
    if (tel_neg.overflow || tel_neg.ineligible) {
        throw std::runtime_error("Negative unary test failed evaluation");
    }
    if (tel_neg.certified_bound > 2) {
        throw std::runtime_error("Lagrangian bound with negative unary exceeds exact q_t");
    }
}

void test_bound_exceeds_k() {
    // Create a complete bipartite graph K_{3,3}
    // S = {0, 1}
    // U = {2, 3, 4, 5}
    // Vertices 0, 1, 2 are partition A, vertices 3, 4, 5 are partition B
    // Edges: {0,3}, {0,4}, {0,5}, {1,3}, {1,4}, {1,5}, {2,3}, {2,4}, {2,5}
    Graph graph(6, {
        {0, 3}, {0, 4}, {0, 5},
        {1, 3}, {1, 4}, {1, 5},
        {2, 3}, {2, 4}, {2, 5}
    });

    LagrangianPrefixBoundEvaluator evaluator(graph);

    // We will use K = 2 to exercise strict threshold-aware early success.
    // Let S = {0, 1}.
    // S has size 2. U has size 4.
    // For t = 2 (T has size 2):
    // If T = {2, 3}: S union T = {0, 1, 2, 3}.
    // S union T has A-part: {0, 1, 2}, B-part: {3}.
    // Edges crossing S union T:
    // Partition A contains all A-vertices {0, 1, 2}.
    // Partition B-part inside S union T is {3}. B-part outside is {4, 5}.
    // Edges crossing are from {0, 1, 2} to {4, 5}.
    // There are 3 * 2 = 6 such edges.
    // If T = {3, 4}: S union T = {0, 1, 3, 4}.
    // A-part inside is {0, 1}, B-part inside is {3, 4}.
    // Edges crossing:
    // - From {0, 1} to {5}: 2 * 1 = 2 edges.
    // - From {2} to {3, 4}: 1 * 2 = 2 edges.
    // Total cut = 4 edges.
    // So exact q_2 = 4.
    // Let's check if the Lagrangian bound can certify a bound >= 4.
    auto tel = evaluator.evaluate(3U, 2U, {2});
    if (tel.certified_bound <= 2 || tel.certified_bound > 4)
        throw std::runtime_error("Lagrangian bound failed strict K=2 certification");
}

} // namespace

int main() {
    try {
        test_signed_ceil_division();
        test_exhaustive_graphs();
        test_empty_full_exclusions();
        test_nonuniform_boundary_terms_and_negative_unary();
        test_bound_exceeds_k();
        std::cout << "All Lagrangian prefix-bound prototype tests passed successfully.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LAGRANGIAN TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
