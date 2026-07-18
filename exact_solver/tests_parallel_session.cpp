#include "parallel_decision_session.hpp"
#include "oracle.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <iostream>

namespace cutwidth {
extern std::atomic<std::int64_t> active_node_count;
}

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
cutwidth::Graph graph_from_bits(std::uint32_t n, std::uint64_t bits) {
    std::vector<std::pair<std::uint32_t,std::uint32_t>> edges;
    std::uint32_t i=0;
    for(std::uint32_t u=0;u<n;++u)for(std::uint32_t v=u+1;v<n;++v,++i)
        if((bits>>i)&1U)edges.emplace_back(u,v);
    return cutwidth::Graph(n,edges);
}
std::string graph_signature(const cutwidth::Graph& graph) {
    std::string result;
    for (cutwidth::Graph::Vertex u = 0; u < graph.size(); ++u)
        for (cutwidth::Graph::Vertex v = u + 1; v < graph.size(); ++v)
            if (graph.adjacent(u, v))
                result += std::to_string(u) + "-" + std::to_string(v) + ";";
    return result;
}

void check(const cutwidth::Graph& graph, std::uint32_t threshold,
           cutwidth::ParallelRuntime runtime = cutwidth::ParallelRuntime::native,
           cutwidth::CacheMode cache_mode = cutwidth::CacheMode::fixed_threshold,
           bool use_cache = true) {
    cutwidth::DecisionOptions options;
    options.cache_mode = cache_mode;
    options.use_failed_state_cache = use_cache;
    options.use_canonical_ownership = false;
    options.failed_state_cache_memory_bytes = 1U << 20;
    options.parallel_runtime = runtime;
    cutwidth::ParallelDecisionSession session(graph,threshold,options,4);
    std::uint64_t donations=0;
    std::size_t maximum_workers=0;
    double busy_worker_seconds=0.0, allocated_worker_seconds=0.0;
    std::uint64_t calls=0;
    while(session.status()==cutwidth::SessionStatus::unresolved){
        const auto event=session.service({32,
            std::chrono::steady_clock::now()+std::chrono::seconds(5)});
        require(event.reason != cutwidth::SessionYieldReason::deadline,
                "parallel quantum lost a worker wakeup and slept to its deadline");
        donations+=event.donations;
        maximum_workers=std::max(maximum_workers,event.workers_used);
        busy_worker_seconds += event.busy_worker_seconds;
        allocated_worker_seconds += event.allocated_worker_seconds;
        require(++calls<100000,"parallel session stopped making progress");
    }
    const auto optimum=cutwidth::oracle::subset_dp(graph).cutwidth;
    const bool feasible=threshold>=optimum;
    require(session.status()==(feasible?cutwidth::SessionStatus::feasible:
                                       cutwidth::SessionStatus::infeasible),
            "parallel persistent decision disagrees with subset DP: n=" +
            std::to_string(graph.size()) + ", threshold=" +
            std::to_string(threshold) + ", optimum=" + std::to_string(optimum) +
            ", status=" + std::to_string(static_cast<int>(session.status())) +
            ", edges=" + graph_signature(graph));
    if(feasible){const auto order=session.ordering();require(graph.validate_ordering(order)&&
        graph.ordering_cutwidth(order)<=threshold,"parallel session returned invalid witness");}
    if(graph.size()>=7 && !feasible && maximum_workers != 0)
        require(donations!=0,
                "parallel session did not exercise concurrent donation: donations=" +
                std::to_string(donations) + ", workers=" +
                std::to_string(maximum_workers));
    require(allocated_worker_seconds >= busy_worker_seconds &&
            busy_worker_seconds >= 0.0,
            "parallel worker-time accounting is inconsistent");
}

void feasible_race_regression(bool use_cache, cutwidth::CacheMode cache_mode,
                              std::uint64_t repetitions) {
    const cutwidth::Graph graph(6, {{0,2},{1,5},{2,3},{2,4},{3,5}});
    for (std::uint64_t repetition = 0; repetition < repetitions; ++repetition)
        check(graph, 2, cutwidth::ParallelRuntime::native, cache_mode, use_cache);
}

void recursive_subtree_worker_equivalence() {
    const cutwidth::Graph graph(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
        {3,4},{4,5},{4,7},{5,6},{6,7}});
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    auto run_partition = [&](std::uint32_t threshold) {
        cutwidth::ShardedDynamicDecisionCache cache(
            graph.word_count(), 8, {0, 1U << 20});
        cutwidth::DecisionOptions options;
        options.cache_mode = cutwidth::CacheMode::cross_threshold;
        options.failed_state_cache_memory_bytes = 1U << 20;
        cutwidth::RecursiveDynamicSubtreeWorker worker(
            graph, threshold, options, cache);
        bool feasible = false;
        for (cutwidth::Graph::Vertex first = 0; first < graph.size(); ++first) {
            for (cutwidth::Graph::Vertex second = 0; second < graph.size(); ++second) {
                if (first == second) continue;
                const auto cut = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(graph.degree(first)) +
                    graph.degree(second) -
                    2 * static_cast<std::uint32_t>(graph.adjacent(first, second)));
                if (graph.degree(first) > threshold || cut > threshold) continue;
                const auto result = worker.run({first, second}, cut);
                require(result.status != cutwidth::DecisionStatus::timed_out,
                        "recursive subtree unexpectedly timed out");
                if (result.status == cutwidth::DecisionStatus::feasible) {
                    require(graph.validate_ordering(result.ordering) &&
                            graph.ordering_cutwidth(result.ordering) <= threshold,
                            "recursive subtree returned invalid witness");
                    feasible = true;
                    break;
                }
            }
            if (feasible) break;
        }
        return feasible;
    };
    require(!run_partition(optimum - 1),
            "recursive subtree partition missed an infeasibility proof");
    require(run_partition(optimum),
            "recursive subtree partition missed a feasible witness");

    cutwidth::ShardedDynamicDecisionCache cache(
        graph.word_count(), 4, {0, 1U << 20});
    cutwidth::RecursiveDynamicSubtreeWorker worker(graph, optimum, {}, cache);
    bool rejected = false;
    try { (void)worker.run({0, 1}, 999); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "recursive subtree accepted a mismatched prefix cut");
    const auto prefix_cut = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(graph.degree(0)) + graph.degree(1) -
        2 * static_cast<std::uint32_t>(graph.adjacent(0, 1)));
    std::atomic<bool> stopped{true};
    require(worker.run({0, 1}, prefix_cut, &stopped).status ==
                cutwidth::DecisionStatus::timed_out &&
            worker.run({0, 1}, prefix_cut, nullptr,
                std::chrono::steady_clock::now()).status ==
                cutwidth::DecisionStatus::timed_out,
            "recursive subtree converted cancellation/deadline into a proof");
    const auto bounded = worker.run(
        {0, 1}, prefix_cut, nullptr,
        std::chrono::steady_clock::time_point::max(), 1);
    require(bounded.status == cutwidth::DecisionStatus::timed_out &&
                bounded.stats.nodes_expanded <= 1,
            "recursive subtree ignored its bounded external work credit");
}

void recursive_coarse_forest_equivalence() {
    const cutwidth::Graph graph(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
        {3,4},{4,5},{4,7},{5,6},{6,7}});
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    for (const auto cache_mode : {cutwidth::CacheMode::fixed_threshold,
                                  cutwidth::CacheMode::cross_threshold})
    for (const auto threshold : {optimum - 1, optimum}) {
        cutwidth::DecisionOptions options;
        options.cache_mode = cache_mode;
        options.canonical_frontier_bootstrap = true;
        options.recursive_coarse_kernel = true;
        options.failed_state_cache_memory_bytes = 1U << 20;
        cutwidth::ParallelDecisionSession session(graph, threshold, options, 2);
        const auto event = session.service({std::numeric_limits<std::uint64_t>::max(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        require(event.status == (threshold == optimum
                    ? cutwidth::SessionStatus::feasible
                    : cutwidth::SessionStatus::infeasible),
                "recursive coarse forest changed the exact decision");
        require(event.workers_used >= 1U &&
                event.terminal_regions != 0,
                "recursive coarse forest did not service compact parallel tasks: workers=" +
                std::to_string(event.workers_used) + ", terminal=" +
                std::to_string(event.terminal_regions) + ", threshold=" +
                std::to_string(threshold) + ", optimum=" +
                std::to_string(optimum));
        if (event.status == cutwidth::SessionStatus::feasible) {
            const auto ordering = session.ordering();
            require(graph.validate_ordering(ordering) &&
                    graph.ordering_cutwidth(ordering) <= threshold,
                    "recursive coarse forest returned invalid witness");
        }
    }

    cutwidth::DecisionOptions resume_options;
    resume_options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    resume_options.canonical_frontier_bootstrap = true;
    resume_options.recursive_coarse_kernel = true;
    resume_options.failed_state_cache_memory_bytes = 1U << 20;
    cutwidth::ParallelDecisionSession paused(
        graph, optimum - 1, resume_options, 2);
    const auto censored = paused.service({
        std::numeric_limits<std::uint64_t>::max(),
        std::chrono::steady_clock::now()});
    require(censored.status == cutwidth::SessionStatus::unresolved &&
            censored.right_censored,
            "coarse recursive deadline was converted into a proof");
    const auto snapshot = paused.quiesce_and_snapshot();
    cutwidth::ParallelDecisionSession resumed(graph, snapshot, resume_options, 2);
    const auto completed = resumed.service({
        std::numeric_limits<std::uint64_t>::max(),
        std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    require(completed.status == cutwidth::SessionStatus::infeasible,
            "coarse recursive pause/resume changed the exact result");

    {
        cutwidth::DecisionOptions options;
        options.cache_mode = cutwidth::CacheMode::fixed_threshold;
        options.canonical_frontier_bootstrap = true;
        options.recursive_coarse_kernel = true;
        options.backend = cutwidth::DecisionBackend::dynamic;
        options.best_next_buckets = true;
        options.failed_state_cache_memory_bytes = 1U << 20;

        cutwidth::ParallelDecisionSession session(graph, optimum - 1, options, 2);
        const auto event = session.service({std::numeric_limits<std::uint64_t>::max(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});

        require(event.status == cutwidth::SessionStatus::infeasible,
                "recursive coarse forest best-next-buckets changed exact decision");
        require(event.delta.best_next_bucket_checks > 0, "No bucket checks in parallel session");
        require(event.delta.best_next_bucket_parent_prunes > 0, "No parent prunes in parallel session");
        require(event.delta.best_next_bucket_candidates_avoided > 0, "No candidates avoided in parallel session");

        // Compare status with buckets off
        cutwidth::DecisionOptions options_off = options;
        options_off.best_next_buckets = false;
        cutwidth::ParallelDecisionSession session_off(graph, optimum - 1, options_off, 2);
        const auto event_off = session_off.service({std::numeric_limits<std::uint64_t>::max(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});

        require(event_off.status == cutwidth::SessionStatus::infeasible,
                "recursive coarse forest buckets off changed exact decision");
        require(event_off.delta.best_next_bucket_checks == 0,
                "Checks recorded in parallel session when option is disabled");
        require(event_off.delta.best_next_bucket_parent_prunes == 0,
                "Prunes recorded in parallel session when option is disabled");
        require(event_off.delta.best_next_bucket_candidates_avoided == 0,
                "Candidates avoided in parallel session when option is disabled");
    }
}

void external_epoch_parallel_equivalence() {
    const cutwidth::Graph graph(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
        {3,4},{4,5},{4,7},{5,6},{6,7}});
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.failed_state_cache_memory_bytes = 1U << 20;
    cutwidth::ParallelDecisionSession session(graph, optimum - 1, options, 2, true);
    cutwidth::ParallelSessionEvent event;
    for (unsigned epoch = 0; epoch != 1000 &&
             session.status() == cutwidth::SessionStatus::unresolved; ++epoch) {
        session.begin_external_epoch(64, std::chrono::steady_clock::now() +
            std::chrono::seconds(1));
        std::thread first([&] { session.run_external_lease(0); });
        std::thread second([&] { session.run_external_lease(1); });
        first.join();
        second.join();
        event = session.finish_external_epoch();
    }
    require(event.status == cutwidth::SessionStatus::infeasible,
            "parallel external leases changed the exact decision");
    const auto snapshot = session.quiesce_and_snapshot();
    require(snapshot.status == cutwidth::SessionStatus::infeasible ||
                snapshot.regions.empty(),
            "external epoch snapshot retained an active lease");
}

void recursive_coarse_explicit_yield() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t row = 0; row < 5; ++row)
        for (std::uint32_t column = 0; column < 5; ++column) {
            const auto vertex = row * 5 + column;
            if (row + 1 < 5) edges.emplace_back(vertex, vertex + 5);
            if (column + 1 < 5) edges.emplace_back(vertex, vertex + 1);
        }
    cutwidth::DecisionOptions options;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.canonical_frontier_bootstrap = true;
    options.recursive_coarse_kernel = true;
    options.failed_state_cache_memory_bytes = 1U << 20;
    const cutwidth::Graph graph(25, edges);
    cutwidth::ParallelDecisionSession session(graph, 4, options, 2);
    cutwidth::ParallelSessionEvent event;
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::thread service_thread([&] {
        entered.store(true, std::memory_order_release);
        event = session.service({std::numeric_limits<std::uint64_t>::max(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        returned.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    while (!returned.load(std::memory_order_acquire)) {
        session.request_yield();
        std::this_thread::yield();
    }
    service_thread.join();
    const bool yielded = event.status == cutwidth::SessionStatus::unresolved &&
        event.reason == cutwidth::SessionYieldReason::yield_requested &&
        event.right_censored;
    const bool completed_before_yield =
        event.status == cutwidth::SessionStatus::infeasible &&
        event.reason == cutwidth::SessionYieldReason::terminal &&
        !event.right_censored;
    require(yielded || completed_before_yield,
            "explicit coarse recursive yield produced an invalid session state");
    const auto snapshot = session.quiesce_and_snapshot();
    if (yielded)
        require(!snapshot.regions.empty(),
                "explicit coarse recursive yield lost the live frontier");
    else
        require(snapshot.status == cutwidth::SessionStatus::infeasible &&
                    snapshot.regions.empty(),
                "completed coarse proof retained a live frontier");
}

void recursive_coarse_quantum_uses_allocated_workers() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t row = 0; row < 5; ++row)
        for (std::uint32_t column = 0; column < 5; ++column) {
            const auto vertex = row * 5 + column;
            if (row + 1 < 5) edges.emplace_back(vertex, vertex + 5);
            if (column + 1 < 5) edges.emplace_back(vertex, vertex + 1);
        }
    const cutwidth::Graph graph(25, edges);
    cutwidth::DecisionOptions options;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.canonical_frontier_bootstrap = true;
    options.recursive_coarse_kernel = true;
    options.failed_state_cache_memory_bytes = 1U << 20;
    cutwidth::ParallelDecisionSession session(graph, 4, options, 4);
    const auto event = session.service({1,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100)});
    require(event.workers_used == 4,
            "one-unit coarse service used " + std::to_string(event.workers_used) +
            " of four allocated workers");
}

void parallel_snapshot_resume() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t row = 0; row < 4; ++row)
        for (std::uint32_t column = 0; column < 4; ++column) {
            const auto vertex = row * 4 + column;
            if (row + 1 < 4) edges.emplace_back(vertex, vertex + 4);
            if (column + 1 < 4) edges.emplace_back(vertex, vertex + 1);
        }
    const cutwidth::Graph graph(16, edges);
    const auto threshold = cutwidth::oracle::subset_dp(graph).cutwidth - 1;
    cutwidth::DecisionOptions options;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.use_canonical_ownership = false;
    options.canonical_frontier_bootstrap = true;
    options.failed_state_cache_memory_bytes = 1U << 20;
    cutwidth::ParallelDecisionSession original(graph, threshold, options, 4);
    const auto snapshot = original.quiesce_and_snapshot();
    require(snapshot.regions.size() >= 2,
            "parallel snapshot test did not create a canonical region forest; regions=" +
            std::to_string(snapshot.regions.size()));
    cutwidth::ParallelDecisionSession resumed(graph, snapshot, options, 4);
    std::uint64_t calls = 0;
    while (original.status() == cutwidth::SessionStatus::unresolved)
        (void)original.service({8,
            std::chrono::steady_clock::now()+std::chrono::seconds(5)});
    while (resumed.status() == cutwidth::SessionStatus::unresolved) {
        (void)resumed.service({8,
            std::chrono::steady_clock::now()+std::chrono::seconds(5)});
        require(++calls < 100000, "restored parallel forest stopped making progress");
    }
    require(original.status() == cutwidth::SessionStatus::infeasible &&
            resumed.status() == original.status(),
            "parallel region-forest resume changed the exact result");

    auto tampered = snapshot;
    tampered.regions.back().parent_region_id = tampered.regions.back().region_id;
    bool rejected = false;
    try { cutwidth::ParallelDecisionSession invalid(graph, tampered, options, 4); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "cyclic parallel region forest was accepted");
}

void unstarted_fragment_claim_lifecycle() {
    const cutwidth::Graph graph(4, {
        {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}});
    cutwidth::DecisionOptions options;
    options.use_partial_bounds = false;
    options.failed_state_cache_memory_bytes = 1U << 20;
    cutwidth::ParallelDecisionSession session(graph, 2, options, 1);

    const auto claim = session.claim_unstarted_fragment();
    require(claim && claim->reservation_id != 0,
            "claim did not return a reservation identity");
    require(!session.inspect_unstarted_fragment() &&
                !session.claim_unstarted_fragment(),
            "claimed fragment remained visible to another consumer");
    bool snapshot_rejected = false;
    try { (void)session.quiesce_and_snapshot(); }
    catch (const std::logic_error&) { snapshot_rejected = true; }
    require(snapshot_rejected, "snapshot accepted an active fragment claim");
    // DFS may service its controller boundary, but it must not run the claim.
    const auto paused = session.service({1,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    require(paused.status == cutwidth::SessionStatus::unresolved &&
                paused.delta.nodes_expanded == 0,
            "DFS ran a claimed unstarted fragment");
    require(!session.retire_unstarted_fragment(*claim),
            "legacy retirement bypassed an active claim");
    auto tampered = *claim;
    ++tampered.reservation_id;
    require(!session.release_claimed_unstarted_fragment(tampered),
            "wrong reservation released a claimed fragment");
    require(session.release_claimed_unstarted_fragment(*claim),
            "exact claimant could not release its fragment");

    const auto released = session.inspect_unstarted_fragment();
    require(released && released->region_id == claim->region_id &&
                released->reservation_id == 0,
            "released fragment did not return to the unstarted forest");
    const auto second = session.claim_unstarted_fragment();
    require(second && second->reservation_id != claim->reservation_id,
            "reclaim reused a stale reservation identity");
    require(!session.retire_claimed_unstarted_fragment(*claim),
            "stale claimant retired a later claim");
    require(session.retire_claimed_unstarted_fragment(*second),
            "exact claimant could not retire a certified-infeasible fragment");
    require(session.status() == cutwidth::SessionStatus::infeasible,
            "claimed retirement did not propagate exact failure through the forest");
}

void deepest_unstarted_fragment_test() {
    const cutwidth::Graph graph(4, {
        {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}});
    cutwidth::DecisionOptions options;
    options.use_partial_bounds = false;
    options.failed_state_cache_memory_bytes = 1U << 20;

    // We will build a manual ParallelDecisionSnapshot with multiple unstarted regions.
    // Each region needs to be a valid unstarted region, matching the rules of unstarted_snapshot_locked.
    cutwidth::ParallelDecisionSnapshot snapshot;
    snapshot.threshold = 2;
    snapshot.status = cutwidth::SessionStatus::unresolved;
    snapshot.ordering = {0, 1, 2, 3};

    // We need multiple regions of different depths:
    // Region 1: ID = 1, depth = 1 (path = {0})
    // Region 2: ID = 2, depth = 3 (path = {0, 1, 2})
    // Region 3: ID = 3, depth = 2 (path = {0, 1})
    // Region 4: ID = 4, depth = 3 (path = {0, 1, 3}) -> tie breaker with Region 2.
    // All descendant regions should have:
    // - status = unresolved
    // - unfinished_regions = 1
    // - external_regions = 0
    // - frames = { SessionFrameSnapshot{} }
    // - pending = {}
    //
    // To satisfy the forest structure validation, we must have exactly one root.
    // We introduce Region 5 as the unique root, and make Regions 1, 2, 3, and 4
    // descendants (specifically children) of Region 5.
    // Thus, Region 5's external_regions will be 4, and its parent_region_id will be 0.
    // All other regions (1, 2, 3, 4) will have parent_region_id = 5.

    auto make_region = [&](std::uint64_t id, const std::vector<cutwidth::Graph::Vertex>& path) {
        cutwidth::ParallelRegionSnapshot region;
        region.region_id = id;
        region.parent_region_id = 5;
        region.session.threshold = 2;
        region.session.status = cutwidth::SessionStatus::unresolved;
        region.session.path = path;
        region.session.ordering = {0, 1, 2, 3};
        region.session.unfinished_regions = 1;
        region.session.external_regions = 0;
        region.session.frames = { cutwidth::SessionFrameSnapshot{} };
        return region;
    };

    // Region 1: ID = 1, path size = 1
    snapshot.regions.push_back(make_region(1, {0}));
    // Region 2: ID = 2, path size = 3
    snapshot.regions.push_back(make_region(2, {0, 1, 2}));
    // Region 3: ID = 3, path size = 2
    snapshot.regions.push_back(make_region(3, {0, 1}));
    // Region 4: ID = 4, path size = 3 (tie depth with Region 2)
    snapshot.regions.push_back(make_region(4, {0, 1, 3}));

    // Root region (Region 5): ID = 5, parent = 0, external_regions = 4 (children are 1, 2, 3, 4)
    cutwidth::ParallelRegionSnapshot root_region;
    root_region.region_id = 5;
    root_region.parent_region_id = 0;
    root_region.session.threshold = 2;
    root_region.session.status = cutwidth::SessionStatus::unresolved;
    root_region.session.path = {};
    root_region.session.ordering = {0, 1, 2, 3};
    root_region.session.unfinished_regions = 4;
    root_region.session.external_regions = 4;
    root_region.session.frames = {}; // empty, waiting for descendants
    snapshot.regions.push_back(std::move(root_region));

    cutwidth::ParallelDecisionSession session(graph, snapshot, options, 1);

    // 1. inspect_unstarted_fragment and claim_unstarted_fragment should return stable-oldest (Region 1).
    const auto inspect_oldest = session.inspect_unstarted_fragment();
    require(inspect_oldest && inspect_oldest->region_id == 1,
            "inspect_unstarted_fragment did not return stable-oldest");

    // 2. inspect_deepest_unstarted_fragment should return maximum depth.
    // Ties should be broken deterministically by lowest region ID (Region 2).
    const auto inspect_deepest = session.inspect_deepest_unstarted_fragment();
    require(inspect_deepest && inspect_deepest->region_id == 2,
            "inspect_deepest_unstarted_fragment did not return maximum depth or broke ties incorrectly");
    require(inspect_deepest->session.path.size() == 3, "deepest fragment path size is incorrect");

    // 3. claim_deepest_unstarted_fragment should reserve exactly the selected node.
    const auto claim_deepest = session.claim_deepest_unstarted_fragment();
    require(claim_deepest && claim_deepest->region_id == 2 && claim_deepest->reservation_id != 0,
            "claim_deepest_unstarted_fragment did not return the correct reservation");

    // 4. Snapshot refusal during an active deepest claim.
    bool snapshot_rejected = false;
    try { (void)session.quiesce_and_snapshot(); }
    catch (const std::logic_error&) { snapshot_rejected = true; }
    require(snapshot_rejected, "snapshot accepted active deepest fragment claim");

    // 5. Subsequent inspect_deepest_unstarted_fragment should return the next deepest (Region 4, path size = 3).
    const auto inspect_next_deepest = session.inspect_deepest_unstarted_fragment();
    require(inspect_next_deepest && inspect_next_deepest->region_id == 4,
            "inspect_deepest_unstarted_fragment after claim did not return the next deepest");

    // 6. Release validation. Release with wrong reservation should fail.
    auto tampered = *claim_deepest;
    ++tampered.reservation_id;
    require(!session.release_claimed_unstarted_fragment(tampered),
            "wrong reservation released a deepest claimed fragment");

    // 7. Release should succeed with correct reservation.
    require(session.release_claimed_unstarted_fragment(*claim_deepest),
            "exact claimant could not release its deepest fragment");

    // 8. Snapshot success after release.
    bool snapshot_succeeded = true;
    try { (void)session.quiesce_and_snapshot(); }
    catch (const std::logic_error&) { snapshot_succeeded = false; }
    require(snapshot_succeeded, "snapshot failed after releasing deepest claim");

    // 9. Re-inspect oldest should still be Region 1.
    const auto inspect_oldest_again = session.inspect_unstarted_fragment();
    require(inspect_oldest_again && inspect_oldest_again->region_id == 1,
            "inspect_unstarted_fragment after release did not return stable-oldest");

    // 10. Re-inspect deepest should be Region 2 (since it was released).
    const auto inspect_deepest_again = session.inspect_deepest_unstarted_fragment();
    require(inspect_deepest_again && inspect_deepest_again->region_id == 2,
            "inspect_deepest_unstarted_fragment after release did not return Region 2");

    // 11. Claim/retire validation. Claim it again and retire it.
    const auto claim_again = session.claim_deepest_unstarted_fragment();
    require(claim_again && claim_again->region_id == 2, "failed to re-claim Region 2");
    require(session.retire_claimed_unstarted_fragment(*claim_again),
            "exact claimant could not retire certified-infeasible fragment");
}

void donate_and_claim_deepest_unstarted_fragment_test() {
    // K4 has cutwidth 4. At threshold 3 its root children are legal but every
    // completion is infeasible, so retiring one child models a sound external
    // certificate without deleting a feasible witness.
    const cutwidth::Graph graph(4, {
        {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}});

    cutwidth::DecisionOptions options;
    options.use_failed_state_cache = false;
    options.use_partial_bounds = false;
    options.use_depth_two_lookahead = false;
    options.use_twin_symmetry = false;
    options.max_proof_regions = 10;
    options.canonical_frontier_bootstrap = false;
    options.recursive_coarse_kernel = false;

    cutwidth::ParallelDecisionSession session(graph, 3, options, 1);

    // 2. Run service with a small budget to materialize, start, and progress the root node session.
    const auto event = session.service(
        {1, std::chrono::steady_clock::now() + std::chrono::milliseconds(500)});
    (void)event;
    require(session.status() == cutwidth::SessionStatus::unresolved,
            "Session should be unresolved after a small bounded service budget");

    // 3. Atomically donate and claim the deepest unstarted sibling from the materialized quiescent root.
    const auto claim = session.donate_and_claim_deepest_unstarted_fragment();
    require(claim.has_value(), "Expected a successfully donated child fragment");
    require(claim->reservation_id != 0, "Reservation ID should be non-zero");

    // The root node (bootstrap region) has an empty path {}.
    // A donated sibling from the active stack must have a deeper path than the root prefix (path size >= 1).
    require(!claim->session.path.empty(), "Donated child should have a deeper path than the root prefix");

    // 4. Require snapshot refusal while a claim is active.
    bool snapshot_refused = false;
    try {
        (void)session.quiesce_and_snapshot();
    } catch (const std::logic_error&) {
        snapshot_refused = true;
    }
    require(snapshot_refused, "Snapshot should be refused while a claimed fragment is outstanding");

    // 5. Release the claimed fragment.
    require(session.release_claimed_unstarted_fragment(*claim), "Should release the claimed fragment");

    // 6. After release, snapshot must succeed.
    bool snapshot_succeeded = true;
    cutwidth::ParallelDecisionSnapshot snapshot;
    try {
        snapshot = session.quiesce_and_snapshot();
    } catch (const std::logic_error&) {
        snapshot_succeeded = false;
    }
    require(snapshot_succeeded, "Snapshot should succeed after releasing the claimed fragment");

    // 7. Restore from snapshot.
    cutwidth::ParallelDecisionSession restored(graph, snapshot, options, 1);

    // 8. The released child is persisted as ordinary unstarted forest work.
    // Claim that existing child rather than attempting a second donation.
    const auto claim_restored = restored.claim_deepest_unstarted_fragment();
    require(claim_restored.has_value(),
            "Should be able to claim the restored donated fragment");

    // Retire the claimed fragment.
    require(restored.retire_claimed_unstarted_fragment(*claim_restored), "Should successfully retire claimed fragment");

    // Run the restored session to completion. Since we retired a necessary sibling, it must finish as infeasible.
    while (restored.status() == cutwidth::SessionStatus::unresolved) {
        (void)restored.service(
            {100, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(restored.status() == cutwidth::SessionStatus::infeasible,
            "Restored session must terminate as infeasible after fragment retirement");
}

void proof_forest_compaction_test() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::uint32_t row = 0; row < 4; ++row)
        for (std::uint32_t column = 0; column < 4; ++column) {
            const auto vertex = row * 4 + column;
            if (row + 1 < 4) edges.emplace_back(vertex, vertex + 4);
            if (column + 1 < 4) edges.emplace_back(vertex, vertex + 1);
        }
    const cutwidth::Graph graph(16, edges);
    const auto threshold = cutwidth::oracle::subset_dp(graph).cutwidth - 1;
    cutwidth::DecisionOptions options;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.use_canonical_ownership = false;
    options.canonical_frontier_bootstrap = true;
    options.failed_state_cache_memory_bytes = 1U << 20;

    const auto start_nodes = cutwidth::active_node_count.load();
    {
        cutwidth::ParallelDecisionSession session(graph, threshold, options, 4);

        // Use a deadline already in the past.  Every worker exits at its next
        // deadline check after picking up at most one node, so the session is
        // guaranteed to remain unresolved while still exercising at least one
        // DFS step, giving compact_proof_forest() terminal nodes to reclaim.
        const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        (void)session.service({std::numeric_limits<std::uint64_t>::max(), past});

        require(session.status() == cutwidth::SessionStatus::unresolved,
                "Test expects session to remain unresolved for compaction check");

        // After reconcile_quiescent_frontier() -> compact_proof_forest(), all
        // terminal nodes have been erased from Impl::nodes, so active_node_count
        // must equal the number of unresolved regions.  Allow +1 for the root
        // node that is unresolved-but-waiting and may not appear in snapshot
        // (snapshot only emits regions that a resumed session would re-enqueue).
        const auto snapshot = session.quiesce_and_snapshot();
        const auto active_count = cutwidth::active_node_count.load() - start_nodes;
        const auto unresolved_count = static_cast<std::int64_t>(snapshot.regions.size());

        require(static_cast<std::int64_t>(active_count) <= unresolved_count + 1,
                "Proof forest compaction failed: active_node_count=" +
                std::to_string(active_count) +
                ", unresolved_regions=" + std::to_string(unresolved_count) +
                " (terminal nodes were not reclaimed at epoch boundary)");
        require(active_count >= 1,
                "Compaction removed all nodes but session is still unresolved");
    }
    const auto end_nodes = cutwidth::active_node_count.load();
    require(end_nodes == start_nodes,
            "Memory leak: active_node_count did not return to start after session destruction");
}

void adaptive_coarse_session_memory_pressure_fallback_test() {
    const cutwidth::Graph graph(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
        {3,4},{4,5},{4,7},{5,6},{6,7}});
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    const auto threshold = optimum - 1;

    const std::size_t cache_bytes = 1024U * 1024U;
    auto governor = std::make_shared<cutwidth::MemoryGovernor>(cache_bytes, 0);

    cutwidth::DecisionOptions options1;
    options1.memory_governor = governor;
    options1.failed_state_cache_memory_bytes = cache_bytes;
    options1.use_failed_state_cache = true;
    options1.recursive_coarse_kernel = true;
    options1.canonical_frontier_bootstrap = true;
    options1.use_canonical_ownership = false;
    options1.cooperative_work_stealing = false;

    cutwidth::ParallelDecisionSession session1(graph, threshold, options1, 2);

    require(governor->stats().committed_lease_bytes == cache_bytes,
            "First session cache lease should be committed");
    require(!governor->stats().memory_pressure,
            "Governor should not report memory pressure yet");

    cutwidth::DecisionOptions options2;
    options2.memory_governor = governor;
    options2.failed_state_cache_memory_bytes = cache_bytes;
    options2.use_failed_state_cache = true;
    options2.recursive_coarse_kernel = true;
    options2.canonical_frontier_bootstrap = true;
    options2.use_canonical_ownership = false;
    options2.cooperative_work_stealing = false;

    cutwidth::ParallelDecisionSession session2(graph, threshold, options2, 2);

    require(governor->stats().committed_lease_bytes == cache_bytes,
            "Memory budget must not be exceeded");
    require(governor->stats().leases_rejected == 1,
            "Second session cache lease should have been rejected");
    require(governor->stats().memory_pressure,
            "Memory pressure should be reported on governor after lease rejection fallback");

    while (session1.status() == cutwidth::SessionStatus::unresolved) {
        session1.service({32, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    while (session2.status() == cutwidth::SessionStatus::unresolved) {
        session2.service({32, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }

    require(session1.status() == cutwidth::SessionStatus::infeasible,
            "Session1 should prove infeasibility");
    require(session2.status() == cutwidth::SessionStatus::infeasible,
            "Session2 should prove infeasibility (correctness parity)");
}

void bootstrap_overshoot_test() {
    const cutwidth::Graph graph(6, {
        {0,1},{0,2},{0,3},{0,4},{0,5},
        {1,2},{1,3},{1,4},{1,5},
        {2,3},{2,4},{2,5},
        {3,4},{3,5},
        {4,5}
    });
    cutwidth::DecisionOptions options;
    options.canonical_frontier_bootstrap = true;
    options.max_proof_regions = 2;
    options.threads = 2;
    options.use_failed_state_cache = false;
    options.use_canonical_ownership = false;

    cutwidth::ParallelDecisionSession session(graph, 5, options, 2);
    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const auto event = session.service({0, past});
    require(event.resolved_proof_regions_bound == 2, "Overshoot resolved bound error");
    require(event.configured_proof_regions_bound == 2, "Overshoot configured bound error");

    while (session.status() == cutwidth::SessionStatus::unresolved) {
        (void)session.service({32, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(session.status() == cutwidth::SessionStatus::infeasible, "Overshoot test incorrect status");
}

void dynamic_suppression_test() {
    const cutwidth::Graph hard(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
        {3,4},{4,5},{4,7},{5,6},{6,7}});
    cutwidth::DecisionOptions options;
    options.canonical_frontier_bootstrap = true;
    options.max_proof_regions = 4;
    options.threads = 4;
    options.use_failed_state_cache = false;
    options.use_canonical_ownership = false;

    cutwidth::ParallelDecisionSession session(hard, 4, options, 4);
    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const auto initial_event = session.service({0, past});
    require(initial_event.resolved_proof_regions_bound == 4, "Resolved bound tracking error");
    require(initial_event.configured_proof_regions_bound == 4, "Configured bound tracking error");

    while (session.status() == cutwidth::SessionStatus::unresolved) {
        (void)session.service({32, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(session.status() == cutwidth::SessionStatus::infeasible, "Suppression test incorrect status");
}

void resume_over_limit_exactness() {
    const cutwidth::Graph hard(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
        {3,4},{4,5},{4,7},{5,6},{6,7}});

    cutwidth::DecisionOptions options1;
    options1.canonical_frontier_bootstrap = true;
    options1.max_proof_regions = 32;
    options1.threads = 4;
    options1.use_failed_state_cache = false;
    options1.use_canonical_ownership = false;

    cutwidth::ParallelDecisionSession session1(hard, 4, options1, 2, true);

    const auto snapshot = session1.quiesce_and_snapshot(cutwidth::SnapshotPolicy::omit_cache);
    require(snapshot.status == cutwidth::SessionStatus::unresolved, "Snapshot should be unresolved");
    require(snapshot.regions.size() > 2, "Snapshot should have > 2 regions to test resume-over-limit");

    cutwidth::DecisionOptions options2;
    options2.canonical_frontier_bootstrap = true;
    options2.max_proof_regions = 2;
    options2.threads = 4;
    options2.use_failed_state_cache = false;
    options2.use_canonical_ownership = false;

    cutwidth::ParallelDecisionSession session2(hard, snapshot, options2, 4);

    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    const auto initial_event2 = session2.service({0, past});
    require(initial_event2.resolved_proof_regions_bound == 2, "Resumed resolved bound error");
    require(initial_event2.configured_proof_regions_bound == 2, "Resumed configured bound error");

    while (session2.status() == cutwidth::SessionStatus::unresolved) {
        (void)session2.service({32, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    }
    require(session2.status() == cutwidth::SessionStatus::infeasible, "Resumed session should prove infeasibility");
}

void test_dfs_min_remaining_vertices_telemetry() {
    const cutwidth::Graph graph(5, {{0,1},{1,2},{2,3},{3,4}});
    {
        cutwidth::DecisionOptions stack_options;
        stack_options.recursive_coarse_kernel = false;
        cutwidth::ParallelDecisionSession session(graph, 0, stack_options, 1);
        const auto event = session.service({std::numeric_limits<std::uint64_t>::max(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        require(event.status == cutwidth::SessionStatus::infeasible, "Should be infeasible");
        require(event.delta.dfs_min_remaining_vertices <= 5, "min remaining vertices should be tracked in stack engine");
        require(event.delta.dfs_min_remaining_vertices > 0, "min remaining vertices should be positive in stack engine");
    }
    {
        cutwidth::DecisionOptions recursive_options;
        recursive_options.cache_mode = cutwidth::CacheMode::fixed_threshold;
        recursive_options.canonical_frontier_bootstrap = true;
        recursive_options.recursive_coarse_kernel = true;
        recursive_options.failed_state_cache_memory_bytes = 1U << 20;

        cutwidth::ParallelDecisionSession session(graph, 0, recursive_options, 1);
        const auto event = session.service({std::numeric_limits<std::uint64_t>::max(),
            std::chrono::steady_clock::now() + std::chrono::seconds(5)});
        require(event.status == cutwidth::SessionStatus::infeasible, "Should be infeasible");
        require(event.delta.dfs_min_remaining_vertices <= 5, "min remaining vertices should be tracked in recursive engine");
        require(event.delta.dfs_min_remaining_vertices > 0, "min remaining vertices should be positive in recursive engine");
    }
}

void test_coarse_recursive_residual_dp_production() {
    const cutwidth::Graph graph(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
        {3,4},{4,5},{4,7},{5,6},{6,7}});
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;

    cutwidth::DecisionOptions options;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.canonical_frontier_bootstrap = true;
    options.recursive_coarse_kernel = true;
    options.dfs_residual_dp_max_remaining = 23;
    options.residual_dp_max_bytes = 1U << 20;
    options.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    options.failed_state_cache_memory_bytes = 1U << 20;

    cutwidth::ParallelDecisionSession session(graph, optimum - 1, options, 2);
    const auto event = session.service({std::numeric_limits<std::uint64_t>::max(),
        std::chrono::steady_clock::now() + std::chrono::seconds(5)});

    require(event.status == cutwidth::SessionStatus::infeasible, "Should be infeasible");
    require(event.delta.residual_dp_attempts > 0 || event.delta.residual_dp_states > 0,
            "residual DP should be attempted or have completed states in coarse-recursive mode");
}

} // namespace

int main(int argc, char** argv){
    if (argc == 3 && std::string(argv[1]) == "--race-regression") {
        const auto mode = std::string(argv[2]);
        feasible_race_regression(mode != "no-cache",
            mode == "cross" ? cutwidth::CacheMode::cross_threshold
                            : cutwidth::CacheMode::fixed_threshold,
            10000);
        return 0;
    }
    try {
        // Nested donations start from a non-empty inherited prefix. Repetition is
        // intentional: the old absolute-depth snapshot bug was schedule-sensitive
        // and could publish a false failed-state proof only under a narrow race.
        feasible_race_regression(true, cutwidth::CacheMode::fixed_threshold, 1000);
        feasible_race_regression(true, cutwidth::CacheMode::cross_threshold, 1000);
        recursive_subtree_worker_equivalence();
        recursive_coarse_forest_equivalence();
        external_epoch_parallel_equivalence();
        recursive_coarse_explicit_yield();
        recursive_coarse_quantum_uses_allocated_workers();
        parallel_snapshot_resume();
        unstarted_fragment_claim_lifecycle();
        deepest_unstarted_fragment_test();
        std::cerr << "donate_and_claim_deepest_unstarted_fragment_test\n";
        donate_and_claim_deepest_unstarted_fragment_test();
        proof_forest_compaction_test();
        adaptive_coarse_session_memory_pressure_fallback_test();
        std::cerr << "bootstrap_overshoot_test\n";
        bootstrap_overshoot_test();
        std::cerr << "dynamic_suppression_test\n";
        dynamic_suppression_test();
        std::cerr << "resume_over_limit_exactness\n";
        resume_over_limit_exactness();
        std::cerr << "test_dfs_min_remaining_vertices_telemetry\n";
        test_dfs_min_remaining_vertices_telemetry();
        std::cerr << "test_coarse_recursive_residual_dp_production\n";
        test_coarse_recursive_residual_dp_production();
        std::mt19937_64 rng(0x51e5510ULL);
        for(std::uint32_t n=2;n<=6;++n){
            for(int sample=0;sample<3;++sample){
                std::uint64_t bits=0;const auto pairs=n*(n-1)/2;
                for(std::uint32_t i=0;i<pairs;++i)if((rng()&3U)==0)bits|=std::uint64_t{1}<<i;
                const auto graph=graph_from_bits(n,bits);
                const auto optimum=cutwidth::oracle::subset_dp(graph).cutwidth;
                check(graph,optimum);
                if(optimum!=0)check(graph,optimum-1);
#ifdef CUTWIDTH_HAVE_ONETBB
                check(graph,optimum,cutwidth::ParallelRuntime::onetbb);
                if(optimum!=0)check(graph,optimum-1,cutwidth::ParallelRuntime::onetbb);
#endif
            }
        }
        const cutwidth::Graph hard(8, {{0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
            {3,4},{4,5},{4,7},{5,6},{6,7}});
        const auto optimum=cutwidth::oracle::subset_dp(hard).cutwidth;
        for (int repetition = 0; repetition < 10; ++repetition)
            check(hard,optimum-1);
#ifdef CUTWIDTH_HAVE_ONETBB
        check(hard,optimum-1,cutwidth::ParallelRuntime::onetbb);
#endif
        check(hard,optimum-1,cutwidth::ParallelRuntime::native,
              cutwidth::CacheMode::cross_threshold);
    } catch (const std::exception& e) {
        std::cerr << "Exception caught: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
