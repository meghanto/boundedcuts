#include "progressive_cheap_bound_session.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace cutwidth;

namespace {

Graph clique_four() {
    return Graph(4, {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}});
}

ParallelDecisionSession unstarted_forest(const Graph& graph, std::uint32_t threshold) {
    DecisionOptions options;
    // The test deliberately leaves this proof fragment unstarted; certified
    // retirement is the cheap-bound arm's responsibility here.
    options.use_partial_bounds = false;
    options.failed_state_cache_memory_bytes = 1U << 20;
    return ParallelDecisionSession(graph, threshold, options, 1);
}

void test_stale_generation_and_prefix_identity_are_rejected() {
    const auto graph = clique_four();
    auto forest = unstarted_forest(graph, 2);
    const auto fragment = forest.inspect_unstarted_fragment();
    assert(fragment);

    ProgressiveCheapBoundSession session(graph, {});
    session.activate_threshold(2, 7);
    assert(session.enqueue(*fragment, 7));
    session.activate_threshold(2, 8);
    const auto stale = session.service_one(forest);
    assert(stale.task && stale.stale_rejected && !stale.fragment_rejected &&
           !stale.certified_prune);
    assert(forest.inspect_unstarted_fragment());

    session.activate_threshold(2, 9);
    assert(session.enqueue(*fragment, 9));
    // The queued prefix must match the exact never-started region, not merely
    // its threshold/generation.  Mutating the caller-owned fragment after
    // admission must therefore reject service rather than retire work.
    auto altered = *fragment;
    altered.session.path.push_back(0);
    ProgressiveCheapBoundSession identity_session(graph, {});
    identity_session.activate_threshold(2, 10);
    assert(identity_session.enqueue(altered, 10));
    const auto rejected = identity_session.service_one(forest);
    assert(rejected.task && rejected.fragment_rejected && !rejected.certified_prune);
    assert(forest.inspect_unstarted_fragment());
}

void test_certified_bound_retires_only_the_unstarted_fragment() {
    const auto graph = clique_four();
    auto forest = unstarted_forest(graph, 2);
    const auto fragment = forest.inspect_unstarted_fragment();
    assert(fragment);
    PartialBoundOptions options;
    // K4's residual-degree session ceiling is only two, so a threshold-two
    // pruning fixture must use an additional certified area bound.
    options.edge_distance_area = true;
    ProgressiveCheapBoundSession session(graph, options);
    session.activate_threshold(2, 1);
    assert(session.enqueue(*fragment, 1));
    const auto event = session.service_one(forest);
    assert(event.task && event.certified_prune && !event.fragment_rejected &&
           event.stats.evaluations != 0);
    assert(!forest.inspect_unstarted_fragment());
    assert(forest.status() == SessionStatus::infeasible);
}

Graph complete_bipartite_3_3() {
    return Graph(6, {
        {0, 3}, {0, 4}, {0, 5},
        {1, 3}, {1, 4}, {1, 5},
        {2, 3}, {2, 4}, {2, 5}
    });
}

Graph large_graph() {
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    for (std::uint32_t i = 0; i < 64; ++i) {
        edges.emplace_back(i, i + 1);
    }
    return Graph(65, edges);
}

void test_default_off() {
    const auto graph = clique_four();
    auto forest = unstarted_forest(graph, 2);
    const auto fragment = forest.inspect_unstarted_fragment();
    assert(fragment);

    PartialBoundOptions options;
    options.lagrangian_prefix_bound = false;

    ProgressiveCheapBoundSession session(graph, options);
    session.activate_threshold(2, 1);
    assert(session.enqueue(*fragment, 1));
    const auto event = session.service_one(forest);
    assert(event.task && !event.lagrangian_evaluated && event.stats.lagrangian_evaluations == 0);
}

void test_sound_prune_retire() {
    const auto graph = complete_bipartite_3_3();
    auto forest = unstarted_forest(graph, 2);
    const auto fragment = forest.inspect_unstarted_fragment();
    assert(fragment);

    PartialBoundOptions options;
    options.lagrangian_prefix_bound = true;
    options.lagrangian_max_slack = 3;
    options.lagrangian_max_residual = 10;
    options.lagrangian_denominator = 2;

    ProgressiveCheapBoundSession session(graph, options);
    session.activate_threshold(2, 1);
    assert(session.enqueue(*fragment, 1));
    const auto event = session.service_one(forest);
    assert(event.task && event.lagrangian_evaluated && event.certified_prune && !event.fragment_rejected);
    assert(event.stats.lagrangian_certified_prunes == 1);
    assert(!forest.inspect_unstarted_fragment());
    assert(forest.status() == SessionStatus::infeasible);
}

void test_inconclusive_release() {
    const auto graph = complete_bipartite_3_3();
    auto forest = unstarted_forest(graph, 100);
    const auto fragment = forest.inspect_unstarted_fragment();
    assert(fragment);

    PartialBoundOptions options;
    options.lagrangian_prefix_bound = true;
    options.lagrangian_max_slack = 100;
    options.lagrangian_max_residual = 10;
    options.lagrangian_denominator = 2;

    ProgressiveCheapBoundSession session(graph, options);
    session.activate_threshold(100, 1);
    assert(session.enqueue(*fragment, 1));
    const auto event = session.service_one(forest);
    assert(event.task && event.lagrangian_evaluated && !event.certified_prune);
    assert(event.stats.lagrangian_certified_prunes == 0);
    assert(forest.inspect_unstarted_fragment());
}

void test_strict_greater_than_k() {
    const auto graph = clique_four();

    {
        auto forest = unstarted_forest(graph, 2);
        const auto fragment = forest.inspect_unstarted_fragment();

        PartialBoundOptions options;
        options.lagrangian_prefix_bound = true;
        options.lagrangian_max_slack = 3;
        options.lagrangian_max_residual = 10;
        options.lagrangian_denominator = 2;

        ProgressiveCheapBoundSession session(graph, options);
        session.activate_threshold(2, 1);
        assert(session.enqueue(*fragment, 1));
        const auto event = session.service_one(forest);
        assert(event.task && event.lagrangian_evaluated);
        assert(event.certified_prune);
    }

    {
        auto forest = unstarted_forest(graph, 3);
        const auto fragment = forest.inspect_unstarted_fragment();

        PartialBoundOptions options;
        options.lagrangian_prefix_bound = true;
        options.lagrangian_max_slack = 3;
        options.lagrangian_max_residual = 10;
        options.lagrangian_denominator = 2;

        ProgressiveCheapBoundSession session(graph, options);
        session.activate_threshold(3, 1);
        assert(session.enqueue(*fragment, 1));
        const auto event = session.service_one(forest);
        assert(event.task && event.lagrangian_evaluated);
        assert(!event.certified_prune);
    }
}

void test_mismatch_behavior() {
    const auto graph = clique_four();
    auto forest = unstarted_forest(graph, 2);
    const auto fragment = forest.inspect_unstarted_fragment();
    assert(fragment);

    PartialBoundOptions options;
    options.lagrangian_prefix_bound = true;
    options.lagrangian_max_slack = 3;
    options.lagrangian_max_residual = 10;
    options.lagrangian_denominator = 2;

    {
        ProgressiveCheapBoundSession session(graph, options);
        session.activate_threshold(2, 1);
        assert(session.enqueue(*fragment, 1));
        session.activate_threshold(2, 2);
        const auto event = session.service_one(forest);
        assert(event.stale_rejected);
        assert(!event.lagrangian_evaluated);
    }

    {
        ProgressiveCheapBoundSession session(graph, options);
        session.activate_threshold(2, 1);
        assert(session.enqueue(*fragment, 1));

        auto modified_fragment = *fragment;
        modified_fragment.session.path.push_back(0);

        const auto claimed = forest.claim_unstarted_fragment();
        assert(claimed);
        const auto event = session.evaluate_claimed_one(*claimed);
        assert(event.fragment_rejected);
        assert(!event.lagrangian_evaluated);
        forest.release_claimed_unstarted_fragment(*claimed);
    }
}

void test_dynamic_mask_support() {
    const auto graph = large_graph();
    assert(graph.word_count() == 2);

    auto forest = unstarted_forest(graph, 1);
    const auto fragment = forest.inspect_unstarted_fragment();
    assert(fragment);

    PartialBoundOptions options;
    options.lagrangian_prefix_bound = true;
    options.lagrangian_max_slack = 3;
    options.lagrangian_max_residual = 100;
    options.lagrangian_denominator = 2;

    ProgressiveCheapBoundSession session(graph, options);
    session.activate_threshold(1, 1);
    assert(session.enqueue(*fragment, 1));
    const auto event = session.service_one(forest);
    assert(event.task && event.lagrangian_evaluated);
}

void test_claim_pairing_regression() {
    const auto graph = clique_four();
    auto forest = unstarted_forest(graph, 2);

    const auto fragment_base = forest.inspect_unstarted_fragment();
    assert(fragment_base);

    // Create fragment A and B which are distinct in region_id and prefix path
    auto fragment_a = *fragment_base;
    fragment_a.region_id = 101;
    fragment_a.session.path = {0};

    auto fragment_b = *fragment_base;
    fragment_b.region_id = 102;
    fragment_b.session.path = {1};

    // Both must be claimed (reservation_id > 0)
    fragment_a.reservation_id = 1;
    fragment_b.reservation_id = 2;

    ProgressiveCheapBoundSession session(graph, {});
    session.activate_threshold(2, 1);

    // Force queued fragment A
    assert(session.enqueue(fragment_a, 1));

    // Try to evaluate B (which is claimed but not yet enqueued)
    auto event_b_before = session.evaluate_claimed_one(fragment_b);
    // Verifies B is evaluated only after its claimed identity is enqueued (returns fragment_rejected, no task)
    assert(!event_b_before.task);
    assert(event_b_before.fragment_rejected);

    // Now enqueue B
    assert(session.enqueue(fragment_b, 1));

    // Evaluate B
    auto event_b_after = session.evaluate_claimed_one(fragment_b);
    assert(event_b_after.task);
    assert(event_b_after.task->region_id == fragment_b.region_id);
    assert(!event_b_after.fragment_rejected);

    // Verifies A never causes B retirement: A is still pending
    assert(session.has_pending());

    // Stale tasks do not starve later matching work.
    // Make A stale by advancing threshold 2 to generation 2
    session.activate_threshold(2, 2);

    // Enqueue new live task B under generation 2
    auto fragment_b_gen2 = fragment_b;
    assert(session.enqueue(fragment_b_gen2, 2));

    // Snapshot/restore
    auto snapshot = session.snapshot();
    ProgressiveCheapBoundSession restored_session(graph, {});
    restored_session.restore(snapshot);

    // Evaluate B in the restored session; stale A must not starve B
    auto event_b_gen2 = restored_session.evaluate_claimed_one(fragment_b_gen2);
    assert(event_b_gen2.task);
    assert(event_b_gen2.task->region_id == fragment_b_gen2.region_id);
    assert(event_b_gen2.task->generation == 2);
    assert(!event_b_gen2.fragment_rejected);
    assert(!event_b_gen2.stale_rejected);

    // Verify there are no pending tasks left
    assert(!restored_session.has_pending());
}

} // namespace

int main() {
    test_stale_generation_and_prefix_identity_are_rejected();
    test_certified_bound_retires_only_the_unstarted_fragment();
    test_default_off();
    test_sound_prune_retire();
    test_inconclusive_release();
    test_strict_greater_than_k();
    test_mismatch_behavior();
    test_dynamic_mask_support();
    test_claim_pairing_regression();
    std::cout << "progressive cheap-bound session tests passed\n";
}
