#include "decision.hpp"
#include "decomposition.hpp"
#include "graph.hpp"
#include "oracle.hpp"
#include "optimizer_v2.hpp"
#include "solver.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cutwidth::DecisionStatus;
using cutwidth::Graph;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Graph graph_from_bits(std::uint32_t n, std::uint64_t bits) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    std::uint32_t edge_index = 0;
    for (std::uint32_t u = 0; u < n; ++u) {
        for (std::uint32_t v = u + 1; v < n; ++v, ++edge_index) {
            if ((bits >> edge_index) & 1U) edges.emplace_back(u, v);
        }
    }
    return Graph(n, edges);
}

Graph complete(std::uint32_t n) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t u = 0; u < n; ++u)
        for (std::uint32_t v = u + 1; v < n; ++v) edges.emplace_back(u, v);
    return Graph(n, edges);
}

void check_all_thresholds(const Graph& graph, const std::string& label) {
    const auto oracle = cutwidth::oracle::subset_dp(graph);
    const std::uint32_t maximum_threshold = graph.edge_count();
    for (std::uint32_t k = 0; k <= maximum_threshold; ++k) {
        const auto result = cutwidth::decide_cutwidth(graph, k);
        const bool expected = k >= oracle.cutwidth;
        require(result.threshold == k, label + ": threshold not echoed");
        require(result.status == (expected ? DecisionStatus::feasible : DecisionStatus::infeasible),
                label + ": wrong answer at threshold " + std::to_string(k));
        if (expected) {
            require(graph.validate_ordering(result.ordering), label + ": invalid witness");
            require(graph.ordering_cutwidth(result.ordering) <= k,
                    label + ": witness exceeds threshold");
        } else {
            require(result.ordering.empty(), label + ": infeasible answer has witness");
        }
        cutwidth::DecisionOptions parallel;
        parallel.threads = 3;
        const auto parallel_result = cutwidth::decide_cutwidth(graph, k, parallel);
        require(parallel_result.status == result.status,
                label + ": parallel answer differs at threshold " + std::to_string(k));
        if (expected) {
            require(graph.validate_ordering(parallel_result.ordering) &&
                    graph.ordering_cutwidth(parallel_result.ordering) <= k,
                    label + ": parallel witness is invalid");
        }

        cutwidth::DecisionOptions bucket_opt;
        bucket_opt.best_next_buckets = true;
        bucket_opt.backend = cutwidth::DecisionBackend::dynamic;
        const auto bucket_result = cutwidth::decide_cutwidth(graph, k, bucket_opt);
        require(bucket_result.status == result.status,
                label + ": best-next-buckets answer differs at threshold " + std::to_string(k));
        if (expected) {
            require(graph.validate_ordering(bucket_result.ordering) &&
                    graph.ordering_cutwidth(bucket_result.ordering) <= k,
                    label + ": best-next-buckets witness is invalid");
        }
    }

    // Failed-state caching must be semantics-preserving.
    cutwidth::DecisionOptions options;
    options.use_failed_state_cache = false;
    const auto below = oracle.cutwidth == 0
        ? cutwidth::decide_cutwidth(graph, 0, options)
        : cutwidth::decide_cutwidth(graph, oracle.cutwidth - 1, options);
    require(below.status == (oracle.cutwidth == 0 ? DecisionStatus::feasible
                                                 : DecisionStatus::infeasible),
            label + ": cache-disabled answer differs");
    cutwidth::DecisionOptions lookahead;
    lookahead.use_depth_two_lookahead = true;
    const auto with_lookahead = cutwidth::decide_cutwidth(graph, oracle.cutwidth, lookahead);
    require(with_lookahead.status == DecisionStatus::feasible,
            label + ": lookahead rejected the optimum threshold");

    // Fail-first is ordering only: both directions must preserve the decision.
    cutwidth::DecisionOptions low_cut_first;
    low_cut_first.use_fail_first_candidate_order = false;
    const auto ordered_reference = cutwidth::decide_cutwidth(
        graph, oracle.cutwidth, low_cut_first);
    require(ordered_reference.status == DecisionStatus::feasible &&
            graph.ordering_cutwidth(ordered_reference.ordering) <= oracle.cutwidth,
            label + ": low-cut-first ordering changed feasibility");
}

void exhaustive_and_random_threshold_tests() {
    // Every labeled simple graph through five vertices, at every meaningful k.
    for (std::uint32_t n = 0; n <= 5; ++n) {
        const std::uint32_t pairs = n * (n - 1) / 2;
        const std::uint64_t graph_count = std::uint64_t{1} << pairs;
        for (std::uint64_t bits = 0; bits < graph_count; ++bits) {
            check_all_thresholds(graph_from_bits(n, bits),
                                 "exhaustive n=" + std::to_string(n) +
                                 " bits=" + std::to_string(bits));
        }
    }

    std::mt19937_64 rng(0xD3C1510ULL);
    for (std::uint32_t n = 6; n <= 10; ++n) {
        for (int sample = 0; sample < 20; ++sample) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
            const auto density = static_cast<std::uint32_t>(5 + 15 * (sample % 7));
            for (std::uint32_t u = 0; u < n; ++u)
                for (std::uint32_t v = u + 1; v < n; ++v)
                    if (rng() % 100 < density) edges.emplace_back(u, v);
            check_all_thresholds(Graph(n, edges), "random n=" + std::to_string(n));
        }
    }
}

void optimization_equivalence_tests() {
    std::mt19937_64 rng(0x0F71A12EULL);
    for (std::uint32_t n = 2; n <= 10; ++n) {
        for (int sample = 0; sample < 10; ++sample) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
            for (std::uint32_t u = 0; u < n; ++u)
                for (std::uint32_t v = u + 1; v < n; ++v)
                    if (rng() % 4 == 0) edges.emplace_back(u, v);
            Graph graph(n, edges);
            const auto oracle = cutwidth::oracle::subset_dp(graph);
            const auto optimized = cutwidth::ExactSolver(graph).solve();
            require(optimized.optimal && optimized.upper_bound == oracle.cutwidth,
                    "optimization disagrees with DP");
            const auto optimized_v2 = cutwidth::optimize_cutwidth_v2(graph);
            require(optimized_v2.optimal &&
                    optimized_v2.lower_bound == oracle.cutwidth &&
                    optimized_v2.upper_bound == oracle.cutwidth &&
                    graph.validate_ordering(optimized_v2.ordering) &&
                    graph.ordering_cutwidth(optimized_v2.ordering) == oracle.cutwidth,
                    "v2 optimization disagrees with DP");
            cutwidth::OptimizerV2Options policy_override;
            policy_override.annealing_min_vertices = 0;
            policy_override.annealing_max_iterations = 0;
            policy_override.descending_feasible_steps_before_binary = 0;
            const auto overridden = cutwidth::optimize_cutwidth_v2(graph, policy_override);
            require(overridden.optimal && overridden.upper_bound == oracle.cutwidth,
                    "named optimizer policy overrides changed exact result");
            cutwidth::DecisionOptions dynamic_options;
            dynamic_options.backend = cutwidth::DecisionBackend::dynamic;
            dynamic_options.threads = 3; // Explicit, documented serial fallback.
            const auto forced_dynamic = cutwidth::decide_cutwidth(
                graph, oracle.cutwidth, dynamic_options);
            require(forced_dynamic.status == DecisionStatus::feasible &&
                    graph.ordering_cutwidth(forced_dynamic.ordering) <= oracle.cutwidth,
                    "forced dynamic backend disagrees with word64 backend");
            dynamic_options.use_fail_first_candidate_order = false;
            const auto dynamic_low_cut_first = cutwidth::decide_cutwidth(
                graph, oracle.cutwidth, dynamic_options);
            require(dynamic_low_cut_first.status == DecisionStatus::feasible &&
                    graph.ordering_cutwidth(dynamic_low_cut_first.ordering) <= oracle.cutwidth,
                    "dynamic low-cut-first ordering changed feasibility");
            const auto yes = cutwidth::decide_cutwidth(graph, optimized.upper_bound);
            require(yes.status == DecisionStatus::feasible, "decision rejects optimum");
            if (optimized.upper_bound > 0) {
                const auto no = cutwidth::decide_cutwidth(graph, optimized.upper_bound - 1);
                require(no.status == DecisionStatus::infeasible,
                        "decision accepts below optimum");
            }
        }
    }
}

void component_and_twin_tests() {
    // Components: K5, path on four vertices, two isolates. max(6,1,0,0) = 6.
    Graph disconnected(11, {{0,1},{0,2},{0,3},{0,4},{1,2},{1,3},{1,4},{2,3},{2,4},{3,4},
                            {5,6},{6,7},{7,8}});
    const auto components = cutwidth::connected_components(disconnected);
    require(components.size() == 4, "wrong component count");
    require(components[0].graph.size() == 5 && components[1].graph.size() == 4,
            "components are not deterministically ordered");
    require(cutwidth::decide_cutwidth(disconnected, 5).status == DecisionStatus::infeasible,
            "disconnected graph accepted below max component optimum");
    require(cutwidth::decide_cutwidth(disconnected, 6).status == DecisionStatus::feasible,
            "disconnected graph rejected at max component optimum");
    const auto disconnected_v2 = cutwidth::optimize_cutwidth_v2(disconnected);
    require(disconnected_v2.optimal && disconnected_v2.upper_bound == 6,
            "component-aware optimizer returned wrong optimum");
    require(cutwidth::decide_cutwidth_v2(disconnected, 5).status == DecisionStatus::infeasible,
            "component-aware decision accepted below optimum");

    // 0,1 are false twins; 2,3 are true twins.
    Graph twins(6, {{0,4},{0,5},{1,4},{1,5},{2,3},{2,4},{3,4}});
    const auto classes = cutwidth::twin_classes(twins);
    require(classes.size() == 2, "wrong twin class count");
    require(classes[0].kind == cutwidth::TwinKind::False &&
            classes[0].vertices == std::vector<Graph::Vertex>({0,1}),
            "false twins not detected");
    require(classes[1].kind == cutwidth::TwinKind::True &&
            classes[1].vertices == std::vector<Graph::Vertex>({2,3}),
            "true twins not detected");
    check_all_thresholds(twins, "mixed twin graph");
    cutwidth::DecisionOptions no_twins;
    no_twins.use_twin_symmetry = false;
    const auto twin_reference = cutwidth::decide_cutwidth(twins, 1, no_twins);
    const auto twin_pruned = cutwidth::decide_cutwidth(twins, 1);
    require(twin_reference.status == twin_pruned.status,
            "twin symmetry changed decision semantics");
}

void timeout_test() {
    // K20 at one below optimum (100) has no witness and enough failed subsets to
    // make a 1 ms deadline observable without relying on an invalid result.
    const Graph graph = complete(20);
    cutwidth::DecisionOptions options;
    options.time_limit = std::chrono::milliseconds(1);
    const auto result = cutwidth::decide_cutwidth(graph, 99, options);
    // Symmetry reductions may prove this instance within the deadline; either
    // a rigorous proof or a timeout is valid, but never a false witness.
    require(result.status != DecisionStatus::feasible,
            "decision accepted a threshold below the clique optimum");
    require(result.ordering.empty(), "non-feasible timeout result has an ordering");
    options.threads = 4;
    options.parallel_min_cache_shards = 3;
    options.parallel_cache_shards_per_thread = 2;
    const auto parallel = cutwidth::decide_cutwidth(graph, 99, options);
    require(parallel.status != DecisionStatus::feasible && parallel.ordering.empty(),
            "parallel timeout produced a false witness");
}

void dense_root_bound_test() {
    const auto clique = complete(20);
    const auto result = cutwidth::optimize_cutwidth_v2(clique);
    require(result.optimal && result.lower_bound == 100 && result.upper_bound == 100,
            "dense bisection root bound did not certify K20");
    require(result.stats.decision_calls == 0,
            "K20 should close at the root without decision search");
}

void heuristic_portfolio_test() {
    const Graph graph(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
                          {3,4},{4,5},{4,7},{5,6},{6,7}});
    const auto oracle = cutwidth::oracle::subset_dp(graph);
    cutwidth::OptimizerV2Options options;
    options.heuristic_search = cutwidth::HeuristicSearch::portfolio;
    options.heuristic_time = std::chrono::milliseconds(20);
    const auto result = cutwidth::optimize_cutwidth_v2(graph, options);
    require(result.optimal && result.upper_bound == oracle.cutwidth &&
            graph.validate_ordering(result.ordering) &&
            graph.ordering_cutwidth(result.ordering) == result.upper_bound,
            "portfolio heuristic changed exact optimization semantics");
    require(result.stats.heuristic_spectral_seeds == 2 &&
            result.stats.heuristic_grasp_constructions != 0,
            "portfolio heuristic did not exercise spectral and GRASP seeds");
}

void dynamic_backend_test() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t v = 1; v < 70; ++v) edges.emplace_back(v - 1, v);
    const Graph path70(70, edges);
    require(!path70.supports_mask(), "70-vertex graph did not select dynamic storage");
    cutwidth::DecisionOptions dynamic_options;
    dynamic_options.threads = 4;
    const auto yes = cutwidth::decide_cutwidth(path70, 1, dynamic_options);
    require(yes.status == DecisionStatus::feasible &&
            path70.validate_ordering(yes.ordering) &&
            path70.ordering_cutwidth(yes.ordering) == 1,
            "dynamic backend failed on P70");
    require(cutwidth::decide_cutwidth(path70, 0).status == DecisionStatus::infeasible,
            "dynamic backend accepted P70 at width zero");
    const auto optimized = cutwidth::optimize_cutwidth_v2(path70);
    require(optimized.optimal && optimized.upper_bound == 1,
            "dynamic optimizer failed on P70");
}

void adaptive_deadline_overshoot_regression_test() {
    constexpr std::uint32_t side = 12;
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    for (std::uint32_t row = 0; row < side; ++row)
        for (std::uint32_t column = 0; column < side; ++column) {
            const auto vertex = row * side + column;
            if (row + 1 < side) edges.emplace_back(vertex, vertex + side);
            if (column + 1 < side) edges.emplace_back(vertex, vertex + 1);
        }
    const Graph graph(side * side, edges);

    cutwidth::OptimizerV2Options options;
    options.controller = cutwidth::ControllerMode::adaptive;
    options.adaptive_arms = {"dfs"};
    options.threads = 8;
    options.time_limit = std::chrono::milliseconds(100);
    options.memory_budget_bytes = 64U << 20;
    options.failed_state_cache_memory_bytes = 32U << 20;

    const auto started = std::chrono::steady_clock::now();
    (void)cutwidth::optimize_cutwidth_v2(graph, options);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    require(elapsed_ms < 300,
            "Adaptive deadline overshoot was too large: " + std::to_string(elapsed_ms) + " ms");
}

Graph grid(std::uint32_t rows, std::uint32_t cols) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t r = 0; r < rows; ++r) {
        for (std::uint32_t c = 0; c < cols; ++c) {
            std::uint32_t v = r * cols + c;
            if (r + 1 < rows) edges.emplace_back(v, v + cols);
            if (c + 1 < cols) edges.emplace_back(v, v + 1);
        }
    }
    return Graph(rows * cols, edges);
}

void test_bucket_minimum_correctness() {
    std::mt19937 rng(1337);
    for (int run = 0; run < 20; ++run) {
        std::int32_t max_deg = 5 + (rng() % 30);
        cutwidth::DeltaHistogram hist;
        hist.init(max_deg);

        std::size_t n = 10 + (rng() % 20);
        std::vector<std::int32_t> deltas;
        std::vector<bool> placed(n, false);
        for (std::size_t i = 0; i < n; ++i) {
            std::int32_t delta = static_cast<std::int32_t>(rng() % (2 * max_deg + 1)) - max_deg;
            deltas.push_back(delta);
            hist.add(delta);
        }

        struct HistoryFrame {
            std::size_t vertex;
            std::int32_t vertex_delta;
            std::vector<std::pair<std::size_t, std::int32_t>> neighbors_with_old_deltas;
        };
        std::vector<HistoryFrame> history;

        auto get_expected_min = [&]() -> std::int32_t {
            std::int32_t m = std::numeric_limits<std::int32_t>::max();
            for (std::size_t i = 0; i < n; ++i) {
                if (!placed[i]) {
                    m = std::min(m, deltas[i]);
                }
            }
            return m;
        };

        for (int step = 0; step < 100; ++step) {
            bool do_push = history.empty() || (rng() % 2 == 0 && history.size() < n);
            if (do_push) {
                std::vector<std::size_t> unplaced_indices;
                for (std::size_t i = 0; i < n; ++i) {
                    if (!placed[i]) unplaced_indices.push_back(i);
                }
                if (unplaced_indices.empty()) {
                    do_push = false;
                } else {
                    std::size_t target = unplaced_indices[rng() % unplaced_indices.size()];
                    HistoryFrame frame;
                    frame.vertex = target;
                    frame.vertex_delta = deltas[target];

                    hist.remove(deltas[target]);
                    placed[target] = true;

                    for (std::size_t i = 0; i < n; ++i) {
                        if (!placed[i] && (deltas[i] - 2 >= -max_deg) && (rng() % 3 == 0)) {
                            frame.neighbors_with_old_deltas.push_back({i, deltas[i]});
                            hist.remove(deltas[i]);
                            deltas[i] -= 2;
                            hist.add(deltas[i]);
                        }
                    }
                    history.push_back(frame);
                }
            }

            if (!do_push && !history.empty()) {
                auto frame = history.back();
                history.pop_back();

                for (auto it = frame.neighbors_with_old_deltas.rbegin(); it != frame.neighbors_with_old_deltas.rend(); ++it) {
                    std::size_t idx = it->first;
                    std::int32_t old_val = it->second;
                    hist.remove(deltas[idx]);
                    deltas[idx] = old_val;
                    hist.add(deltas[idx]);
                }

                placed[frame.vertex] = false;
                hist.add(frame.vertex_delta);
            }

            std::int32_t expected = get_expected_min();
            std::int32_t actual = hist.get_min_delta();
            require(expected == actual,
                "DeltaHistogram minimum mismatch during simulation: expected=" +
                std::to_string(expected) + ", actual=" + std::to_string(actual) +
                ", run=" + std::to_string(run) + ", step=" + std::to_string(step));
        }
    }
}

void test_best_next_buckets_production() {
    Graph g = grid(3, 4);

    cutwidth::DecisionOptions options;
    options.best_next_buckets = true;
    options.backend = cutwidth::DecisionBackend::dynamic;

    const auto result = cutwidth::decide_cutwidth(g, 3, options);

    require(result.status == DecisionStatus::infeasible, "Direct decision not infeasible");
    require(result.stats.best_next_bucket_checks > 0, "No bucket checks recorded");
    require(result.stats.best_next_bucket_parent_prunes > 0, "No parent prunes recorded");
    require(result.stats.best_next_bucket_candidates_avoided > 0, "No candidates avoided");

    cutwidth::DecisionOptions options_off;
    options_off.best_next_buckets = false;
    options_off.backend = cutwidth::DecisionBackend::dynamic;

    const auto result_off = cutwidth::decide_cutwidth(g, 3, options_off);
    require(result_off.status == DecisionStatus::infeasible, "Direct decision (buckets off) not infeasible");
    require(result_off.stats.best_next_bucket_checks == 0, "Checks recorded when option is disabled");
    require(result_off.stats.best_next_bucket_parent_prunes == 0, "Prunes recorded when option is disabled");
    require(result_off.stats.best_next_bucket_candidates_avoided == 0, "Candidates avoided when option is disabled");
}

} // namespace

int main() {
    try {
        test_bucket_minimum_correctness();
        test_best_next_buckets_production();
        exhaustive_and_random_threshold_tests();
        optimization_equivalence_tests();
        component_and_twin_tests();
        timeout_test();
        dense_root_bound_test();
        heuristic_portfolio_test();
        dynamic_backend_test();
        adaptive_deadline_overshoot_regression_test();
        std::cout << "All exact-core v2 tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "V2 TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
