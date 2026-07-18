#include "decision_session.hpp"
#include "oracle.hpp"

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

cutwidth::Graph graph_from_bits(std::uint32_t n, std::uint64_t bits) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    std::uint32_t index = 0;
    for (std::uint32_t u = 0; u < n; ++u)
        for (std::uint32_t v = u + 1; v < n; ++v, ++index)
            if ((bits >> index) & 1U) edges.emplace_back(u, v);
    return cutwidth::Graph(n, edges);
}

void run_to_completion(const cutwidth::Graph& graph, std::uint32_t threshold,
                       std::uint64_t quantum) {
    cutwidth::DecisionOptions options;
    options.backend = cutwidth::DecisionBackend::dynamic;
    cutwidth::DecisionSession session(graph, threshold, options);
    std::uint64_t services = 0;
    while (session.status() == cutwidth::SessionStatus::unresolved) {
        const auto event = session.service({quantum,
            std::chrono::steady_clock::now() + std::chrono::seconds(10)});
        require(event.status != cutwidth::SessionStatus::cancelled, "session cancelled itself");
        require(++services < 1000000, "session failed to make progress");
    }
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    const bool feasible = threshold >= optimum;
    require(session.status() == (feasible ? cutwidth::SessionStatus::feasible
                                         : cutwidth::SessionStatus::infeasible),
            "persistent decision disagrees with subset DP");
    if (feasible) {
        require(graph.validate_ordering(session.ordering()), "persistent witness is not an ordering");
        require(graph.ordering_cutwidth(session.ordering()) <= threshold,
                "persistent witness exceeds threshold");
    }
}

void exhaustive_forced_yields() {
    for (std::uint32_t n = 0; n <= 4; ++n) {
        const auto pairs = n * (n - 1) / 2;
        for (std::uint64_t bits = 0; bits < (std::uint64_t{1} << pairs); ++bits) {
            const auto graph = graph_from_bits(n, bits);
            const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
            run_to_completion(graph, optimum, 1);
            if (optimum != 0) run_to_completion(graph, optimum - 1, 1);
        }
    }
}

void yield_at_every_observed_safe_point() {
    const auto graph = graph_from_bits(5, 0x16d);
    const auto threshold = cutwidth::oracle::subset_dp(graph).cutwidth - 1;
    cutwidth::DecisionSession baseline(graph, threshold);
    std::uint64_t maximum_safe_point = 0;
    baseline.set_safe_point_hook([&](std::uint64_t point) {
        maximum_safe_point = std::max(maximum_safe_point, point);
    });
    while (baseline.status() == cutwidth::SessionStatus::unresolved)
        (void)baseline.service({std::numeric_limits<std::uint64_t>::max(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    require(maximum_safe_point != 0 &&
            baseline.status() == cutwidth::SessionStatus::infeasible,
            "safe-point baseline did not produce an exact proof");

    for (std::uint64_t target = 1; target <= maximum_safe_point; ++target) {
        cutwidth::DecisionSession interrupted(graph, threshold);
        bool requested = false;
        interrupted.set_safe_point_hook([&](std::uint64_t point) {
            if (!requested && point == target) {
                requested = true;
                interrupted.request_yield();
            }
        });
        const auto event = interrupted.service({std::numeric_limits<std::uint64_t>::max(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        require(requested && event.status == cutwidth::SessionStatus::unresolved &&
                event.reason == cutwidth::SessionYieldReason::yield_requested,
                "forced safe-point yield was not reported as censored");
        while (interrupted.status() == cutwidth::SessionStatus::unresolved)
            (void)interrupted.service({std::numeric_limits<std::uint64_t>::max(),
                std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        require(interrupted.status() == cutwidth::SessionStatus::infeasible,
                "forced safe-point yield changed the exact decision");
    }
}

void resume_does_not_restart() {
    const auto graph = graph_from_bits(6, 0x5a2d);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionSession session(graph, optimum - 1);
    const auto first = session.service({3, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(first.status == cutwidth::SessionStatus::unresolved, "test quantum unexpectedly completed");
    const auto nodes = session.stats().nodes_expanded;
    const auto depth = session.stack_depth();
    const auto second = session.service({3, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(session.stats().nodes_expanded > nodes, "resume repeated or made no progress");
    require(depth != 0 && second.delta.nodes_expanded != 0, "resume lost its continuation");
    while (session.status() == cutwidth::SessionStatus::unresolved)
        (void)session.service({7, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(session.status() == cutwidth::SessionStatus::infeasible, "resumed proof is wrong");
}

void deadline_is_censored() {
    const auto graph = graph_from_bits(6, 0x5a2d);
    cutwidth::DecisionSession session(graph, 0);
    const auto event = session.service({1000, std::chrono::steady_clock::now()});
    require(event.status == cutwidth::SessionStatus::unresolved && event.right_censored &&
            event.reason == cutwidth::SessionYieldReason::deadline,
            "deadline was converted into a proof");
}

void donation_partitions_without_losing_proof() {
    const auto graph = graph_from_bits(7, 0x15a2d);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionSession session(graph, optimum - 1);
    (void)session.service({4, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    const auto nodes_before = session.stats().nodes_expanded;
    require(session.donate_unexplored_sibling(), "hard session had no sibling to donate");
    require(session.unfinished_regions() == 2 && session.pending_continuations() == 1,
            "donation did not transactionally add one region");
    while (session.status() == cutwidth::SessionStatus::unresolved)
        (void)session.service({3, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(session.status() == cutwidth::SessionStatus::infeasible,
            "partitioned infeasibility proof is wrong");
    require(session.unfinished_regions() == 0 && session.pending_continuations() == 0,
            "terminal region accounting is not empty");
    require(session.stats().nodes_expanded > nodes_before,
            "donated continuation was not serviced");

    cutwidth::DecisionSession feasible(graph, optimum);
    (void)feasible.service({2, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    if (feasible.status() == cutwidth::SessionStatus::unresolved)
        (void)feasible.donate_unexplored_sibling();
    while (feasible.status() == cutwidth::SessionStatus::unresolved)
        (void)feasible.service({2, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(feasible.status() == cutwidth::SessionStatus::feasible &&
            graph.ordering_cutwidth(feasible.ordering()) <= optimum,
            "partitioned feasible search lost its witness");
}

void descendant_proofs_survive_donation() {
    const cutwidth::Graph graph(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
        {3,4},{4,5},{4,7},{5,6},{6,7}});
    const auto threshold = cutwidth::oracle::subset_dp(graph).cutwidth - 1;
    cutwidth::DecisionOptions options;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.failed_state_cache_memory_bytes = 1U << 20;
    options.use_canonical_ownership = false;
    cutwidth::DecisionSession session(graph, threshold, options);
    (void)session.service({2,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(session.donate_unexplored_sibling(),
            "cache-publication test could not donate a sibling");
    require(session.extract_pending_continuation().has_value(),
            "cache-publication test could not detach its donated region");
    const auto records_before = session.stats().failed_states_recorded;
    std::uint64_t calls = 0;
    while (!session.waiting_for_external_regions() &&
           session.status() == cutwidth::SessionStatus::unresolved) {
        (void)session.service({8,
            std::chrono::steady_clock::now() + std::chrono::seconds(1)});
        require(++calls < 100000, "donor region stopped making progress");
    }
    require(session.stats().failed_states_recorded > records_before,
            "donation suppressed complete descendant failed-state proofs");
}

void duplicate_region_waits_for_verified_proof() {
    const cutwidth::Graph graph(6, {{0,1},{0,3},{1,2},{1,4},{2,3},{2,5},
        {3,4},{4,5}});
    const auto threshold = cutwidth::oracle::subset_dp(graph).cutwidth - 1;
    auto cache = std::make_shared<cutwidth::ShardedFixedThresholdDynamicCache>(
        1, 4, threshold, cutwidth::DecisionCacheOptions{0, 1U << 20});
    auto ownership = std::make_shared<cutwidth::CanonicalOwnershipTable>(1, 4, 64);
    cutwidth::DecisionOptions first_options;
    first_options.shared_fixed_cache = cache;
    first_options.canonical_ownership = ownership;
    first_options.ownership_id = 1;
    cutwidth::DecisionOptions duplicate_options = first_options;
    duplicate_options.ownership_id = 2;
    cutwidth::DecisionSession owner(graph, threshold, first_options);
    cutwidth::DecisionSession duplicate(graph, threshold, duplicate_options);
    (void)owner.service({1,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    const auto parked = duplicate.service({8,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(parked.status == cutwidth::SessionStatus::unresolved &&
            parked.reason == cutwidth::SessionYieldReason::ownership_wait,
            "duplicate region did not park behind its canonical owner");
    while (owner.status() == cutwidth::SessionStatus::unresolved)
        (void)owner.service({16,
            std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    const auto ready = ownership->take_ready_waiters();
    require(ready.size() == 1 && ready.front() == 2,
            "canonical proof did not wake the parked region");
    while (duplicate.status() == cutwidth::SessionStatus::unresolved)
        (void)duplicate.service({16,
            std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(duplicate.status() == cutwidth::SessionStatus::infeasible &&
            duplicate.stats().failed_cache_hits != 0,
            "woken duplicate did not consume the verified proof");
}

void snapshot_resume_equivalence() {
    const auto graph = graph_from_bits(7, 0x15a2d);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionSession original(graph, optimum - 1);
    (void)original.service({5, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    (void)original.donate_unexplored_sibling();
    (void)original.service({4, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    const auto snapshot = original.quiesce_and_snapshot();
    cutwidth::DecisionSession resumed(graph, snapshot);
    require(resumed.threshold() == original.threshold() &&
            resumed.stack_depth() == original.stack_depth() &&
            resumed.unfinished_regions() == original.unfinished_regions() &&
            resumed.pending_continuations() == original.pending_continuations(),
            "restored session lost continuation structure");
    while (original.status() == cutwidth::SessionStatus::unresolved)
        (void)original.service({3, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    while (resumed.status() == cutwidth::SessionStatus::unresolved)
        (void)resumed.service({3, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(original.status() == resumed.status() &&
            resumed.status() == cutwidth::SessionStatus::infeasible,
            "snapshot resume changed the exact decision");

    auto tampered = snapshot;
    tampered.unfinished_regions++;
    bool rejected = false;
    try { cutwidth::DecisionSession invalid(graph, tampered); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "tampered session region accounting was accepted");
}

void inapplicable_residual_bound_has_zero_state_cost() {
    const cutwidth::Graph path(6, {{0,1},{1,2},{2,3},{3,4},{4,5}});
    cutwidth::DecisionOptions options;
    options.use_partial_bounds = true;
    options.partial_bounds.residual_degree = true;
    options.partial_bounds.edge_distance_area = false;
    options.partial_bounds.degree_distance_area = false;
    options.partial_bounds.degeneracy = false;
    cutwidth::DecisionSession session(path, 1, options);
    while (session.status() == cutwidth::SessionStatus::unresolved)
        (void)session.service({3,
            std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(session.status() == cutwidth::SessionStatus::feasible,
            "path decision unexpectedly failed");
    require(session.stats().partial_bounds.residual_degree_evaluations == 0 &&
            session.stats().residual_histogram_updates == 0 &&
            session.stats().partial_bounds.residual_degree_session_ceiling_skips == 1,
            "inapplicable residual bound retained hot-path state cost");
}

void test_one_state_quantum_persistence() {
    const auto graph = graph_from_bits(6, 0x15a2dULL);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    options.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    options.dfs_residual_dp_max_remaining = 23;
    options.use_failed_state_cache = false;
    cutwidth::DecisionSession session(graph, optimum, options);

    // Service with a quantum of 1 (DP states count towards budget)
    auto event = session.service({1, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    require(session.status() == cutwidth::SessionStatus::unresolved, "session resolved too early");
    require(event.reason == cutwidth::SessionYieldReason::quantum_complete, "reason should be quantum_complete");
    require(session.stats().residual_dp_states == 1, "exactly one DP state should be processed");
    require(session.stats().residual_dp_attempts == 1, "exactly one DP attempt should be recorded");
    require(session.stats().residual_dp_peak_bytes > 0, "residual DP peak bytes should be non-zero");
    require(session.stats().residual_dp_seconds >= 0.0, "residual DP seconds should be non-negative");

    // Resume and complete
    while (session.status() == cutwidth::SessionStatus::unresolved) {
        session.service({100, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(session.status() == cutwidth::SessionStatus::feasible, "should be feasible");
    require(session.stats().residual_dp_states > 1, "should have processed more DP states");
}

void test_infeasible_prune_cache_publication() {
    const auto graph = graph_from_bits(6, 0x15a2dULL);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    options.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    options.dfs_residual_dp_max_remaining = 23;
    options.use_failed_state_cache = true;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.failed_state_cache_memory_bytes = 1U << 19;

    cutwidth::DecisionSession session(graph, optimum - 1, options);
    while (session.status() == cutwidth::SessionStatus::unresolved) {
        session.service({100, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(session.status() == cutwidth::SessionStatus::infeasible, "should be infeasible");
    require(session.stats().residual_dp_infeasible_prunes == 1, "should have pruned 1 infeasible tail");
    require(session.stats().failed_states_recorded == 1, "should have published the failed state to cache");
}

void test_feasible_witness() {
    const auto graph = graph_from_bits(6, 0x15a2dULL);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    options.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    options.dfs_residual_dp_max_remaining = 23;
    options.use_failed_state_cache = false;

    cutwidth::DecisionSession session(graph, optimum, options);
    while (session.status() == cutwidth::SessionStatus::unresolved) {
        session.service({100, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(session.status() == cutwidth::SessionStatus::feasible, "should be feasible");
    require(session.stats().residual_dp_feasible_witnesses == 1, "should have 1 feasible witness from DP");
    require(session.stats().residual_dp_completed_tails >= 1, "should have completed at least 1 tail");
    require(graph.validate_ordering(session.ordering()), "witness should be valid");
    require(graph.ordering_cutwidth(session.ordering()) <= optimum, "witness cutwidth should be correct");
}

void test_memory_rejection_fallback() {
    const auto graph = graph_from_bits(6, 0x15a2dULL);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    // Case A: Memory governor with 1 byte budget
    options.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1, 0);
    options.dfs_residual_dp_max_remaining = 23;
    options.use_failed_state_cache = false;

    cutwidth::DecisionSession session_gov(graph, optimum, options);
    while (session_gov.status() == cutwidth::SessionStatus::unresolved) {
        session_gov.service({100, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(session_gov.status() == cutwidth::SessionStatus::feasible, "should still resolve to feasible");
    require(session_gov.stats().residual_dp_governor_or_cap_rejections >= 1, "should have at least 1 governor/cap rejection");
    require(session_gov.stats().residual_dp_admissions == 0, "should have 0 DP admissions");
    require(session_gov.stats().nodes_expanded > 0, "should have expanded nodes via ordinary DFS");

    // Case B: Byte cap exceeded
    options.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    options.residual_dp_max_bytes = 10;

    cutwidth::DecisionSession session_cap(graph, optimum, options);
    while (session_cap.status() == cutwidth::SessionStatus::unresolved) {
        session_cap.service({100, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(session_cap.status() == cutwidth::SessionStatus::feasible, "should still resolve to feasible");
    require(session_cap.stats().residual_dp_governor_or_cap_rejections >= 1, "should have at least 1 governor/cap rejection");
    require(session_cap.stats().residual_dp_admissions == 0, "should have 0 DP admissions");
}

void test_snapshot_cold_restart() {
    const auto graph = graph_from_bits(6, 0x15a2dULL);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    options.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    options.dfs_residual_dp_max_remaining = 23;
    options.use_failed_state_cache = false;

    cutwidth::DecisionSession original(graph, optimum, options);
    // Start DP and yield immediately
    original.service({1, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    require(original.status() == cutwidth::SessionStatus::unresolved, "should yield");

    // Snapshot it
    const auto snapshot = original.quiesce_and_snapshot();

    // Restore
    cutwidth::DecisionSession restored(graph, snapshot, options);
    // Service to completion
    while (restored.status() == cutwidth::SessionStatus::unresolved) {
        restored.service({100, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(restored.status() == cutwidth::SessionStatus::feasible, "restored should be feasible");
    require(restored.stats().residual_dp_cold_restarts == 1, "should count 1 cold restart");
}

void test_dynamic_dfs_diagnostics_are_accounting_only() {
    const auto graph = graph_from_bits(6, 0x15a2dULL);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    options.backend = cutwidth::DecisionBackend::dynamic;
    options.collect_dfs_diagnostics = true;
    options.dfs_residual_dp_max_remaining = 0;
    const auto result = cutwidth::decide_cutwidth(graph, optimum - 1, options);
    require(result.status == cutwidth::DecisionStatus::infeasible,
            "diagnostic search changed the exact decision");
    const auto& diagnostics = result.stats.dfs_diagnostics;
    require(diagnostics.enabled && diagnostics.threshold == optimum - 1,
            "diagnostic metadata was not retained");
    require(diagnostics.nodes_entered == result.stats.nodes_expanded,
            "diagnostic node total differs from solver node total");
    std::uint64_t depth_nodes = 0;
    std::uint64_t slack_observations = 0;
    for (const auto& depth : diagnostics.by_depth) {
        depth_nodes += depth.nodes_entered;
        for (const auto count : depth.slack_histogram)
            slack_observations += count;
        std::uint64_t child_histogram_observations = 0;
        for (const auto count : depth.viable_child_histogram)
            child_histogram_observations += count;
        require(child_histogram_observations == depth.viable_child_observations,
                "viable-child histogram lost observations");
    }
    require(depth_nodes == diagnostics.nodes_entered &&
            slack_observations == diagnostics.nodes_entered,
            "depth or slack accounting lost nodes");
    std::uint64_t root_nodes = 0;
    for (const auto& root : diagnostics.by_root) root_nodes += root.nodes_entered;
    require(root_nodes == diagnostics.nodes_entered,
            "root-branch accounting lost nodes");
}

void test_delta_bucket_candidate_equivalence() {
    std::mt19937_64 random(0x5eed1234ULL);
    bool observed_index_gather = false;
    bool observed_cross_check = false;
    for (std::uint32_t n = 2; n <= 7; ++n) {
        const auto pairs = n * (n - 1) / 2;
        const auto mask = pairs == 64 ? std::numeric_limits<std::uint64_t>::max()
                                      : (std::uint64_t{1} << pairs) - 1;
        for (std::uint32_t sample = 0; sample < 24; ++sample) {
            const auto graph = graph_from_bits(n, random() & mask);
            const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
            std::vector<std::uint32_t> thresholds{optimum};
            if (optimum != 0) thresholds.push_back(optimum - 1);
            for (const auto threshold : thresholds) {
                cutwidth::DecisionOptions options;
                options.backend = cutwidth::DecisionBackend::dynamic;
                options.dfs_residual_dp_max_remaining = 0;
                options.use_partial_bounds = false;
                options.best_next_buckets = true;

                options.candidate_enumerator = cutwidth::CandidateEnumerator::scan;
                const auto scanned = cutwidth::decide_cutwidth(graph, threshold, options);
                options.candidate_enumerator = cutwidth::CandidateEnumerator::delta_buckets;
                const auto indexed = cutwidth::decide_cutwidth(graph, threshold, options);
                options.candidate_enumerator = cutwidth::CandidateEnumerator::cross_check;
                const auto checked = cutwidth::decide_cutwidth(graph, threshold, options);

                require(scanned.status == indexed.status && indexed.status == checked.status,
                        "candidate enumerators disagree on decision status");
                require(scanned.ordering == indexed.ordering && indexed.ordering == checked.ordering,
                        "candidate enumerators changed deterministic child order");
                require(scanned.stats.nodes_expanded == indexed.stats.nodes_expanded &&
                        indexed.stats.nodes_expanded == checked.stats.nodes_expanded,
                        "candidate enumerators changed deterministic node count");
                require(scanned.stats.failed_cache_queries == indexed.stats.failed_cache_queries &&
                        scanned.stats.failed_cache_hits == indexed.stats.failed_cache_hits &&
                        scanned.stats.failed_states_recorded == indexed.stats.failed_states_recorded,
                        "candidate enumerators changed cache outcomes");
                require(indexed.stats.candidate_scan_checks == 0,
                        "delta-bucket path fell back to the full scan");
                observed_index_gather = observed_index_gather ||
                    indexed.stats.candidate_index_gathers != 0;
                require(indexed.stats.candidate_index_forward_updates ==
                        indexed.stats.candidate_index_rollback_updates,
                        "delta-bucket rollback did not reverse every forward move");
                observed_cross_check = observed_cross_check ||
                    checked.stats.candidate_index_cross_checks != 0;
            }
        }
    }
    require(observed_index_gather,
            "equivalence suite never exercised indexed candidate generation");
    require(observed_cross_check,
            "equivalence suite never compared indexed and scanned candidates");
}

struct SessionRun {
    cutwidth::SessionStatus status = cutwidth::SessionStatus::unresolved;
    cutwidth::DecisionStats stats;
};

SessionRun run_resumable_candidate_mode(
    const cutwidth::Graph& graph, std::uint32_t threshold,
    cutwidth::CandidateEnumerator enumerator) {
    cutwidth::DecisionOptions options;
    options.backend = cutwidth::DecisionBackend::dynamic;
    options.dfs_residual_dp_max_remaining = 0;
    options.use_partial_bounds = false;
    options.candidate_enumerator = enumerator;
    cutwidth::DecisionSession session(graph, threshold, options);
    std::uint64_t services = 0;
    bool donated = false;
    while (session.status() == cutwidth::SessionStatus::unresolved) {
        (void)session.service({1,
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        ++services;
        if (!donated && services == 3) donated = session.donate_unexplored_sibling();
        if (session.status() == cutwidth::SessionStatus::unresolved && services % 5 == 0) {
            const auto snapshot = session.quiesce_and_snapshot();
            session = cutwidth::DecisionSession(graph, snapshot, options);
        }
        require(services < 1000000, "resumable candidate test stopped making progress");
    }
    return {session.status(), session.stats()};
}

void test_resumable_delta_bucket_candidate_equivalence() {
    std::mt19937_64 random(0x51de7b0cULL);
    bool observed_gather = false;
    bool observed_cross_check = false;
    for (std::uint32_t n = 4; n <= 7; ++n) {
        const auto pairs = n * (n - 1) / 2;
        const auto mask = (std::uint64_t{1} << pairs) - 1;
        for (std::uint32_t sample = 0; sample < 12; ++sample) {
            const auto graph = graph_from_bits(n, random() & mask);
            const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
            if (optimum == 0) continue;
            const auto scanned = run_resumable_candidate_mode(
                graph, optimum - 1, cutwidth::CandidateEnumerator::scan);
            const auto indexed = run_resumable_candidate_mode(
                graph, optimum - 1, cutwidth::CandidateEnumerator::delta_buckets);
            const auto checked = run_resumable_candidate_mode(
                graph, optimum - 1, cutwidth::CandidateEnumerator::cross_check);
            require(scanned.status == cutwidth::SessionStatus::infeasible &&
                    indexed.status == scanned.status && checked.status == scanned.status,
                    "resumable candidate enumerators disagree on exact status");
            require(scanned.stats.nodes_expanded == indexed.stats.nodes_expanded &&
                    indexed.stats.nodes_expanded == checked.stats.nodes_expanded,
                    "resumable candidate enumerator changed deterministic node count");
            require(scanned.stats.failed_cache_queries == indexed.stats.failed_cache_queries &&
                    scanned.stats.failed_cache_hits == indexed.stats.failed_cache_hits &&
                    scanned.stats.failed_states_recorded == indexed.stats.failed_states_recorded,
                    "resumable candidate enumerator changed cache outcomes");
            require(indexed.stats.candidate_scan_checks == 0,
                    "resumable delta-bucket path fell back to a full scan");
            require(indexed.stats.candidate_index_forward_updates ==
                    indexed.stats.candidate_index_rollback_updates,
                    "resumable delta-bucket rollback did not reverse every update");
            observed_gather = observed_gather || indexed.stats.candidate_index_gathers != 0;
            observed_cross_check = observed_cross_check ||
                checked.stats.candidate_index_cross_checks != 0;
        }
    }
    require(observed_gather, "resumable suite never gathered indexed candidates");
    require(observed_cross_check,
            "resumable suite never cross-checked indexed candidates");
}

void test_local_continuation_exactness() {
    std::mt19937_64 random(0x10ca1b0uLL);
    bool observed_call = false;
    bool observed_prune = false;
    bool observed_cross_check = false;
    for (std::uint32_t n = 2; n <= 7; ++n) {
        const auto pairs = n * (n - 1) / 2;
        const auto mask = (std::uint64_t{1} << pairs) - 1;
        for (std::uint32_t sample = 0; sample < 32; ++sample) {
            const auto graph = graph_from_bits(n, random() & mask);
            const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
            std::vector<std::uint32_t> thresholds{optimum};
            if (optimum != 0) thresholds.push_back(optimum - 1);
            for (const auto threshold : thresholds) {
                cutwidth::DecisionOptions baseline_options;
                baseline_options.backend = cutwidth::DecisionBackend::dynamic;
                baseline_options.dfs_residual_dp_max_remaining = 0;
                baseline_options.use_partial_bounds = false;
                baseline_options.candidate_enumerator =
                    cutwidth::CandidateEnumerator::delta_buckets;
                const auto baseline = cutwidth::decide_cutwidth(
                    graph, threshold, baseline_options);

                auto probe_options = baseline_options;
                probe_options.candidate_enumerator =
                    cutwidth::CandidateEnumerator::cross_check;
                probe_options.local_continuation_depth = 3;
                probe_options.local_continuation_max_slack =
                    std::numeric_limits<std::uint32_t>::max();
                probe_options.local_continuation_max_children =
                    std::numeric_limits<std::uint32_t>::max();
                probe_options.local_continuation_max_states = 0;
                const auto probed = cutwidth::decide_cutwidth(
                    graph, threshold, probe_options);
                require(probed.status == baseline.status,
                        "local continuation changed exact decision status");
                if (probed.status == cutwidth::DecisionStatus::feasible) {
                    require(graph.validate_ordering(probed.ordering) &&
                            graph.ordering_cutwidth(probed.ordering) <= threshold,
                            "local continuation produced an invalid witness");
                }
                observed_call = observed_call ||
                    probed.stats.local_continuation_calls != 0;
                observed_prune = observed_prune ||
                    probed.stats.local_continuation_parent_prunes != 0;
                observed_cross_check = observed_cross_check ||
                    probed.stats.local_continuation_cross_checks != 0;
                require(probed.stats.local_continuation_inconclusive == 0,
                        "unlimited local continuation became inconclusive");
            }
        }
    }
    require(observed_call, "local continuation gate never admitted a probe");
    require(observed_prune, "local continuation suite never proved a parent failure");
    require(observed_cross_check,
            "local continuation suite never cross-checked a probe state");
}

void test_resumable_local_continuation() {
    const auto graph = graph_from_bits(7, 0x15a2d);
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    require(optimum != 0, "resumable local continuation fixture is trivial");
    cutwidth::DecisionOptions options;
    options.backend = cutwidth::DecisionBackend::dynamic;
    options.dfs_residual_dp_max_remaining = 0;
    options.use_partial_bounds = false;
    options.candidate_enumerator = cutwidth::CandidateEnumerator::cross_check;
    options.local_continuation_depth = 3;
    options.local_continuation_max_slack =
        std::numeric_limits<std::uint32_t>::max();
    options.local_continuation_max_children =
        std::numeric_limits<std::uint32_t>::max();
    options.local_continuation_max_states = 0;
    cutwidth::DecisionSession session(graph, optimum - 1, options);
    std::uint64_t services = 0;
    while (session.status() == cutwidth::SessionStatus::unresolved) {
        (void)session.service({1,
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        ++services;
        if (session.status() == cutwidth::SessionStatus::unresolved &&
            services % 3 == 0) {
            const auto snapshot = session.quiesce_and_snapshot();
            session = cutwidth::DecisionSession(graph, snapshot, options);
        }
        require(services < 1000000,
                "resumable local continuation stopped making progress");
    }
    require(session.status() == cutwidth::SessionStatus::infeasible,
            "resumable local continuation changed exact status");
    require(session.stats().local_continuation_calls != 0 &&
            session.stats().local_continuation_cross_checks != 0,
            "resumable local continuation was not exercised");
}
}

int main() {
    try {
        exhaustive_forced_yields();
        yield_at_every_observed_safe_point();
        resume_does_not_restart();
        deadline_is_censored();
        donation_partitions_without_losing_proof();
        descendant_proofs_survive_donation();
        duplicate_region_waits_for_verified_proof();
        snapshot_resume_equivalence();
        inapplicable_residual_bound_has_zero_state_cost();
        test_one_state_quantum_persistence();
        test_infeasible_prune_cache_publication();
        test_feasible_witness();
        test_memory_rejection_fallback();
        test_snapshot_cold_restart();
        test_dynamic_dfs_diagnostics_are_accounting_only();
        test_delta_bucket_candidate_equivalence();
        test_resumable_delta_bucket_candidate_equivalence();
        test_local_continuation_exactness();
        test_resumable_local_continuation();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
