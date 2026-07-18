#include "graph.hpp"
#include "oracle.hpp"
#include "solver.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cutwidth::ExactSolver;
using cutwidth::Graph;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Graph path(std::uint32_t n) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t v = 1; v < n; ++v) edges.emplace_back(v - 1, v);
    return Graph(n, edges);
}

Graph complete(std::uint32_t n) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t u = 0; u < n; ++u)
        for (std::uint32_t v = u + 1; v < n; ++v) edges.emplace_back(u, v);
    return Graph(n, edges);
}

void check_solver(const Graph& graph, std::uint32_t expected, const std::string& name) {
    const auto result = ExactSolver(graph).solve();
    require(result.optimal, name + ": solver did not certify optimality");
    require(result.lower_bound == expected, name + ": wrong lower bound");
    require(result.upper_bound == expected, name + ": wrong upper bound");
    require(graph.validate_ordering(result.ordering), name + ": invalid ordering");
    require(graph.ordering_cutwidth(result.ordering) == expected,
            name + ": reported ordering has wrong cutwidth");
}

void deterministic_tests() {
    check_solver(Graph(0, {}), 0, "empty graph");
    check_solver(Graph(1, {}), 0, "single vertex");
    check_solver(Graph(7, {}), 0, "edgeless graph");
    check_solver(path(8), 1, "path");
    check_solver(complete(5), 6, "K5");

    // K1,4 has optimum 2: place two leaves, then the center, then two leaves.
    Graph star(5, {{0, 1}, {0, 2}, {0, 3}, {0, 4}});
    check_solver(star, 2, "star");

    const auto brute = cutwidth::oracle::brute_force(complete(6));
    const auto dp = cutwidth::oracle::subset_dp(complete(6));
    require(brute.cutwidth == 9 && dp.cutwidth == 9, "oracle K6 regression");

    std::istringstream labeled_input("isolated\nleft right\nright tail\n");
    const auto labeled = Graph::read_edge_list(labeled_input);
    require(labeled.size() == 4 && labeled.edge_count() == 2,
            "plain parser did not preserve a labeled isolate");
    check_solver(labeled, 1, "labeled input with isolate");

    std::istringstream bundled_input("name: isolate-test\n5 1\n1 2\n");
    const auto bundled = Graph::read_edge_list(bundled_input);
    require(bundled.size() == 5 && bundled.edge_count() == 1,
            "bundled parser did not preserve declared isolates");
    check_solver(bundled, 1, "bundled input with isolates");

    // A numeric edge must not be silently mistaken for an n/m header.
    std::istringstream ambiguous_numeric("3 2\n0 2\n1 2\n");
    const auto numeric_edges = Graph::read_edge_list(ambiguous_numeric);
    require(numeric_edges.size() == 4 && numeric_edges.edge_count() == 3,
            "plain numeric edge was misclassified as a header");
    check_solver(numeric_edges, 2, "ambiguous numeric edge list");
}

void randomized_cross_checks() {
    std::mt19937_64 rng(0xC07D1D7ULL);
    for (std::uint32_t n = 2; n <= 9; ++n) {
        for (int instance = 0; instance < 12; ++instance) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
            const std::uint32_t threshold = 10 + (instance % 5) * 20;
            for (std::uint32_t u = 0; u < n; ++u) {
                for (std::uint32_t v = u + 1; v < n; ++v) {
                    if (rng() % 100 < threshold) edges.emplace_back(u, v);
                }
            }
            Graph graph(n, edges);
            const auto dp = cutwidth::oracle::subset_dp(graph);
            const auto solved = ExactSolver(graph).solve();
            require(solved.optimal, "random instance was not certified");
            require(solved.lower_bound == dp.cutwidth && solved.upper_bound == dp.cutwidth,
                    "solver disagrees with subset DP");
            require(graph.validate_ordering(solved.ordering), "random invalid ordering");
            require(graph.ordering_cutwidth(solved.ordering) == dp.cutwidth,
                    "random incumbent value mismatch");

            // The transposition table is only an optimization. Disabling it
            // must not change the certified optimum.
            if (n <= 8 && instance < 3) {
                cutwidth::SolverOptions no_table;
                no_table.use_transposition_table = false;
                const auto without_table = ExactSolver(graph, no_table).solve();
                require(without_table.optimal && without_table.upper_bound == dp.cutwidth,
                        "solver without dominance disagrees with subset DP");
            }

            // Factorial cross-checks are deliberately confined to n <= 8.
            if (n <= 8 && instance < 4) {
                const auto brute = cutwidth::oracle::brute_force(graph);
                require(brute.cutwidth == dp.cutwidth, "brute force disagrees with subset DP");
            }
        }
    }
}

}  // namespace

int main() {
    try {
        deterministic_tests();
        randomized_cross_checks();
        std::cout << "All cutwidth tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
