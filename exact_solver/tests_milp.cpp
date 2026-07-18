#include "milp_adapter.hpp"
#include "optimizer_v2.hpp"
#include "oracle.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        const cutwidth::Graph path(3, {{0, 1}, {1, 2}});
        std::ostringstream model;
        cutwidth::write_cutwidth_lp(model, path, {{1}});
        const std::string lp = model.str();
        require(lp.find("assign_v0: + x_0_0 + x_0_1 + x_0_2 = 1") != std::string::npos,
                "MILP assignment constraints missing");
        require(lp.find("xor_a_0_0: + z_0_0 - y_0_0 + y_1_0 >= 0") != std::string::npos,
                "MILP crossing constraints missing");
        require(lp.find("decision_limit: + W <= 1") != std::string::npos,
                "MILP decision constraint missing");
        require(lp.find("reversal:") != std::string::npos,
                "MILP reversal symmetry constraint missing");
        cutwidth::MilpModelOptions custom_anchors;
        custom_anchors.reversal_first_vertex = 1;
        custom_anchors.reversal_second_vertex = 2;
        std::ostringstream custom_model;
        cutwidth::write_cutwidth_lp(custom_model, path, custom_anchors);
        require(custom_model.str().find("1 x_1_1 - 1 x_2_1") != std::string::npos,
                "custom reversal anchors were ignored");
        require(lp.find("Binary\n x_0_0") != std::string::npos,
                "MILP binary declarations missing");

        const auto optimal = cutwidth::parse_highs_output(
            "Model status        : Optimal\nPrimal bound      7\n");
        require(optimal.status == cutwidth::MilpStatus::optimal && optimal.optimum == 7,
                "strict optimal status parsing failed");
        require(cutwidth::parse_highs_output(
                    "Model status : Infeasible\n").status == cutwidth::MilpStatus::infeasible,
                "strict infeasible status parsing failed");
        require(cutwidth::parse_highs_output(
                    "Model status : Time limit reached\nPrimal bound 7\n").status ==
                    cutwidth::MilpStatus::limit,
                "limit status was incorrectly certified");
        require(cutwidth::parse_highs_output(
                    "Model status : Optimal\nPrimal bound 7.25\n").status ==
                    cutwidth::MilpStatus::unknown,
                "fractional objective was incorrectly certified");

        const auto backend = cutwidth::run_highs(path, {{1}}, 0.01);
        require(backend.status == cutwidth::MilpStatus::unavailable ||
                backend.status == cutwidth::MilpStatus::optimal ||
                backend.status == cutwidth::MilpStatus::infeasible ||
                backend.status == cutwidth::MilpStatus::limit,
                "optional HiGHS adapter returned an invalid status");
        const auto construction_limit = cutwidth::run_highs(path, {}, 1e-9);
        require(construction_limit.status == cutwidth::MilpStatus::limit ||
                construction_limit.status == cutwidth::MilpStatus::unavailable,
                "normal MILP budget exhaustion was reported as an error");
        if (backend.status != cutwidth::MilpStatus::unavailable) {
            const auto exact = cutwidth::run_highs(path);
            require(exact.status == cutwidth::MilpStatus::optimal && exact.optimum == 1 &&
                    path.validate_ordering(exact.ordering) &&
                    path.ordering_cutwidth(exact.ordering) == 1,
                    "HiGHS optimization did not return a verified exact witness");
            const auto impossible = cutwidth::run_highs(path, {{0}});
            require(impossible.status == cutwidth::MilpStatus::infeasible,
                    "HiGHS decision model failed to certify infeasibility");
            const cutwidth::Graph clique4(4, {{0,1},{0,2},{0,3},{1,2},{1,3},{2,3}});
            const auto clique_exact = cutwidth::run_highs(clique4);
            require(clique_exact.status == cutwidth::MilpStatus::optimal &&
                    clique_exact.optimum == 4 &&
                    clique4.ordering_cutwidth(clique_exact.ordering) == 4,
                    "HiGHS MILP disagrees on K4");
            // Exhaustively ensure reversal symmetry preserves the optimum and
            // the opt/opt-1 decision boundary for every graph through n=4.
            for (std::uint32_t n = 2; n <= 4; ++n) {
                const std::uint32_t pairs = n * (n - 1) / 2;
                for (std::uint64_t bits = 0; bits < (std::uint64_t{1} << pairs); ++bits) {
                    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
                    std::uint32_t edge_index = 0;
                    for (std::uint32_t u = 0; u < n; ++u)
                        for (std::uint32_t v = u + 1; v < n; ++v, ++edge_index)
                            if ((bits >> edge_index) & 1U) edges.emplace_back(u, v);
                    const cutwidth::Graph graph(n, edges);
                    const auto oracle = cutwidth::oracle::subset_dp(graph);
                    const auto strengthened = cutwidth::run_highs(graph);
                    require(strengthened.status == cutwidth::MilpStatus::optimal &&
                            strengthened.optimum == oracle.cutwidth,
                            "reversal symmetry changed exhaustive optimum");
                    if (oracle.cutwidth > 0) {
                        const auto below = cutwidth::run_highs(graph, {{oracle.cutwidth - 1}});
                        require(below.status == cutwidth::MilpStatus::infeasible,
                                "reversal symmetry changed exhaustive decision boundary");
                    }
                }
            }
        }
        cutwidth::OptimizerV2Options integrated_options;
        integrated_options.milp_time_seconds = 1.0;
        const auto integrated = cutwidth::optimize_cutwidth_v2(path, integrated_options);
        require(integrated.optimal && integrated.upper_bound == 1 &&
                integrated.stats.milp_attempted,
                "optimizer MILP root-oracle integration failed");
        require(integrated.stats.milp_status == cutwidth::MilpStatus::optimal ||
                integrated.stats.milp_status == cutwidth::MilpStatus::unavailable,
                "optimizer did not report optional MILP availability clearly");
        std::cout << "All optional MILP adapter tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MILP TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
