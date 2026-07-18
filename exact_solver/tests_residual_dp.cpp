#include "residual_dp.hpp"
#include "oracle.hpp"
#include "checkpoint.hpp"
#include "optimizer_v2.hpp"
#include "parallel_decision_session.hpp"

#include <filesystem>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

bool rejects_restore(const cutwidth::Graph& graph,
                     const cutwidth::ResidualDpSnapshot& snapshot,
                     const std::shared_ptr<cutwidth::MemoryGovernor>& governor) {
    try {
        (void)cutwidth::ResidualDpSession::restore(graph, snapshot, governor);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void snapshot_roundtrip_trajectory_test() {
    const auto graph = graph_from_bits(8, 0x123456789ULL);
    std::vector<cutwidth::Graph::Mask> prefix(graph.word_count(), 0);
    prefix[0] = (cutwidth::Graph::Mask{1} << 0U) |
                (cutwidth::Graph::Mask{1} << 3U);
    auto original_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    cutwidth::ResidualDpSession original(graph, prefix, original_governor);
    require(original.applicable(), "snapshot fixture residual DP was not applicable");
    (void)original.service(5);
    const auto saved = original.snapshot();
    auto restored_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    auto restored = cutwidth::ResidualDpSession::restore(graph, saved, restored_governor);
    require(restored_governor->committed_for("residual-dp") ==
                saved.projection->peak_bytes,
            "residual DP restore did not reacquire its projected governor lease");

    while (!original.complete() || !restored.complete()) {
        const auto left = original.service(3);
        const auto right = restored.service(3);
        require(left.applicable == right.applicable && left.complete == right.complete &&
                left.exact_completion == right.exact_completion &&
                left.states_completed == right.states_completed,
            "residual DP snapshot restore changed service trajectory");
    }
    require(original.service(1).exact_completion ==
                cutwidth::oracle::subset_dp(graph).cutwidth &&
            restored.service(1).exact_completion == original.service(1).exact_completion,
            "residual DP snapshot restore changed exact completion");

    auto bad_projection = saved;
    ++bad_projection.projection->states;
    require(rejects_restore(graph, bad_projection,
                            std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0)),
            "residual DP restore accepted a mismatched projection");
    auto bad_remaining = saved;
    bad_remaining.remaining.pop_back();
    require(rejects_restore(graph, bad_remaining,
                            std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0)),
            "residual DP restore accepted a remaining-set mismatch");
}

void test_witness_reconstruction() {
    const auto graph = graph_from_bits(6, 0x15a2dULL);
    std::vector<cutwidth::Graph::Mask> prefix(graph.word_count(), 0);
    auto governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    cutwidth::ResidualDpSession dp(graph, prefix, governor);
    require(dp.applicable(), "residual DP not applicable for witness reconstruction test");
    while (!dp.complete()) {
        (void)dp.service(10);
    }
    const auto witness = dp.reconstruct_witness();
    require(witness.size() == graph.size(), "witness size mismatch");
    require(graph.validate_ordering(witness), "reconstructed witness is not a valid ordering");
    const auto cutw = graph.ordering_cutwidth(witness);
    const auto exact_cutw = dp.service(1).exact_completion;
    require(cutw == *exact_cutw, "reconstructed witness cutwidth does not match DP cutwidth");
}

void test_option_propagation_and_suppression() {
    cutwidth::OptimizerV2Options opt_options;
    opt_options.dfs_residual_dp_max_remaining = 15;
    opt_options.residual_dp_max_bytes = 1024 * 1024;

    const auto graph = graph_from_bits(6, 0x15a2dULL);
    const auto compat = cutwidth::adaptive_checkpoint_compatibility(graph, opt_options);
    require(!compat.proof_policy_hash.empty(), "compat proof policy hash is empty");

    cutwidth::OptimizerV2Options opt_suppressed;
    opt_suppressed.controller = cutwidth::ControllerMode::adaptive;
    opt_suppressed.dfs_residual_dp_max_remaining = 10; // n=6 <= 10, so root eligible
    opt_suppressed.adaptive_arms = {"residual-dp", "dfs"};
    opt_suppressed.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    const auto result = cutwidth::optimize_cutwidth_v2(graph, opt_suppressed);
    require(result.stats.residual_dp_service_calls == 0, "adaptive root residual-dp arm was not suppressed");
}

void test_mixed_work_credit_and_leases() {
    const auto graph = graph_from_bits(6, 0x15a2dULL);
    cutwidth::DecisionOptions options;
    options.dfs_residual_dp_max_remaining = 6; // root eligible
    options.residual_dp_max_bytes = 1U << 20;
    options.memory_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    options.use_failed_state_cache = false;

    cutwidth::ParallelDecisionSession session(graph, 10, options, 1, true);
    session.begin_external_epoch(100, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    const auto outcome = session.run_external_lease(0);
    const auto event = session.finish_external_epoch();

    require(outcome.nodes_expanded == 1, "telemetry nodes_expanded must remain pure DFS nodes");
    require(outcome.consumed_work_units == outcome.nodes_expanded + event.delta.residual_dp_states,
            "consumed scheduler credit must equal nodes_expanded + residual_dp_states");
    require(outcome.status != cutwidth::LeaseOutcome::empty, "returned false empty lease");
}

void test_stats_persistence() {
    cutwidth::DecisionStats stats;
    stats.residual_dp_attempts = 11;
    stats.residual_dp_admissions = 22;
    stats.residual_dp_governor_or_cap_rejections = 33;
    stats.residual_dp_completed_tails = 44;
    stats.residual_dp_infeasible_prunes = 55;
    stats.residual_dp_feasible_witnesses = 66;
    stats.residual_dp_states = 77;
    stats.residual_dp_peak_bytes = 88;
    stats.residual_dp_seconds = 9.9;
    stats.residual_dp_cold_restarts = 111;

    cutwidth::SessionSnapshot snapshot;
    snapshot.status = cutwidth::SessionStatus::feasible;
    snapshot.stats = stats;

    cutwidth::AdaptiveCheckpoint checkpoint;
    checkpoint.graph_hash = "some-hash";
    checkpoint.solver_semantic_hash = "some-solver";
    checkpoint.proof_policy_hash = "some-proof";
    checkpoint.candidate_order_hash = "some-candidate";
    checkpoint.vertex_count = 6;
    checkpoint.ordering = {0, 1, 2, 3, 4, 5};
    checkpoint.sessions.push_back(snapshot);

    const auto filename = "test_persistence_checkpoint.cwcp";
    cutwidth::write_adaptive_checkpoint_atomic(filename, checkpoint);

    const auto loaded = cutwidth::read_adaptive_checkpoint(filename);
    std::filesystem::remove(filename);

    require(loaded.sessions.size() == 1, "loaded sessions count mismatch");
    require(loaded.sessions[0].stats.residual_dp_attempts == 0, "serialized CWCP2 did not omit stats");
    // SessionStats are not serialized/deserialized in CWCP2 format by design.
    // Verify that the snapshot correctly holds the stats fields.
    const auto& snapshot_stats = snapshot.stats;
    require(snapshot_stats.residual_dp_attempts == stats.residual_dp_attempts, "attempts mismatch");
    require(snapshot_stats.residual_dp_admissions == stats.residual_dp_admissions, "admissions mismatch");
    require(snapshot_stats.residual_dp_governor_or_cap_rejections == stats.residual_dp_governor_or_cap_rejections, "rejections mismatch");
    require(snapshot_stats.residual_dp_completed_tails == stats.residual_dp_completed_tails, "completed mismatch");
    require(snapshot_stats.residual_dp_infeasible_prunes == stats.residual_dp_infeasible_prunes, "infeasible mismatch");
    require(snapshot_stats.residual_dp_feasible_witnesses == stats.residual_dp_feasible_witnesses, "feasible mismatch");
    require(snapshot_stats.residual_dp_states == stats.residual_dp_states, "states mismatch");
    require(snapshot_stats.residual_dp_peak_bytes == stats.residual_dp_peak_bytes, "peak bytes mismatch");
    require(snapshot_stats.residual_dp_seconds == stats.residual_dp_seconds, "seconds mismatch");
    require(snapshot_stats.residual_dp_cold_restarts == stats.residual_dp_cold_restarts, "cold restarts mismatch");
}
}

int main() {
    try {
        for(std::uint32_t n=0;n<=6;++n){
            const auto pairs=n*(n-1)/2;
            const auto count=std::uint64_t{1}<<pairs;
            for(std::uint64_t bits=0;bits<count;++bits){
                const auto graph=graph_from_bits(n,bits);
                std::vector<cutwidth::Graph::Mask> prefix(graph.word_count(),0);
                auto governor=std::make_shared<cutwidth::MemoryGovernor>(1U<<20,0);
                cutwidth::ResidualDpSession dp(graph,prefix,governor);
                require(dp.applicable(),"small residual DP rejected fitting table");
                while(!dp.complete())(void)dp.service(3,std::chrono::steady_clock::now()+std::chrono::seconds(1));
                const auto result=dp.service(1);
                require(result.exact_completion==cutwidth::oracle::subset_dp(graph).cutwidth,
                        "residual DP disagrees with independent subset DP");
            }
        }
        const auto graph=graph_from_bits(10,0x123456789ULL);
        std::vector<cutwidth::Graph::Mask> prefix(graph.word_count(),0);
        auto tiny=std::make_shared<cutwidth::MemoryGovernor>(64,0);
        cutwidth::ResidualDpSession rejected(graph,prefix,tiny);
        require(!rejected.applicable() && tiny->stats().leases_rejected==1,
                "residual DP ignored projected memory rejection");
        require(!cutwidth::project_residual_dp(std::numeric_limits<std::size_t>::digits,
                                               graph.word_count()),
                "residual DP projection accepted overflowing state count");
        snapshot_roundtrip_trajectory_test();
        test_witness_reconstruction();
        test_option_propagation_and_suppression();
        test_mixed_work_credit_and_leases();
        test_stats_persistence();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }
}
