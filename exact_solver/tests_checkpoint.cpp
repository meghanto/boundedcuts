#include "checkpoint.hpp"
#include "optimizer_v2.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <chrono>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void require_rejected(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

cutwidth::Checkpoint sample() {
    cutwidth::Checkpoint checkpoint;
    checkpoint.graph_hash = "fnv1a64:0123456789abcdef";
    checkpoint.solver_hash = "source:solver-v1";
    checkpoint.options_hash = "fnv1a64:fedcba9876543210";
    checkpoint.ordering = {3, 1, 0, 2};
    checkpoint.lower_bound = 7;
    checkpoint.upper_bound = 8;
    checkpoint.elapsed_milliseconds = 12345;
    checkpoint.completed_thresholds = {
        {6, cutwidth::CompletedThresholdResult::infeasible},
        {8, cutwidth::CompletedThresholdResult::feasible},
    };
    checkpoint.timed_out = true;
    return checkpoint;
}

void round_trip_test() {
    const auto checkpoint = sample();
    const auto encoded = cutwidth::serialize_checkpoint(checkpoint);
    require(encoded.rfind("CWCP1\n", 0) == 0, "checkpoint lacks version marker");
    require(cutwidth::parse_checkpoint(encoded) == checkpoint, "checkpoint round trip changed state");
    require(cutwidth::serialize_checkpoint(cutwidth::parse_checkpoint(encoded)) == encoded,
            "checkpoint encoding is not deterministic");
}

void corruption_tests() {
    const auto encoded = cutwidth::serialize_checkpoint(sample());
    require_rejected([&] { (void)cutwidth::parse_checkpoint(encoded.substr(0, encoded.size() - 1)); },
                     "truncated checkpoint accepted");
    require_rejected([&] { (void)cutwidth::parse_checkpoint("CWCP9\n" + encoded.substr(6)); },
                     "unknown checkpoint version accepted");
    require_rejected([&] {
        auto bad = encoded;
        bad += "surprise=1\n";
        (void)cutwidth::parse_checkpoint(bad);
    }, "unknown field accepted");
    require_rejected([&] {
        auto bad = encoded;
        const auto position = bad.find("upper_bound=");
        bad.insert(position, "lower_bound=7\n");
        (void)cutwidth::parse_checkpoint(bad);
    }, "duplicate field accepted");
    require_rejected([&] {
        auto bad = encoded;
        const auto begin = bad.find("solver_hash=");
        bad.erase(begin, bad.find('\n', begin) - begin + 1);
        (void)cutwidth::parse_checkpoint(bad);
    }, "missing field accepted");
    require_rejected([&] {
        auto bad = encoded;
        bad.replace(bad.find("lower_bound=7"), 13, "lower_bound=09");
        (void)cutwidth::parse_checkpoint(bad);
    }, "non-canonical integer accepted");
    require_rejected([&] {
        auto bad = encoded;
        bad.replace(bad.find("timed_out=1"), 11, "timed_out=true");
        (void)cutwidth::parse_checkpoint(bad);
    }, "non-canonical boolean accepted");
    require_rejected([&] {
        auto bad = encoded;
        const auto position = bad.find("graph_hash=") + std::string("graph_hash=").size();
        bad[position] = '9';
        (void)cutwidth::parse_checkpoint(bad);
    }, "bad length prefix accepted");
}

void invariant_tests() {
    auto checkpoint = sample();
    checkpoint.lower_bound = 9;
    require_rejected([&] { cutwidth::validate_checkpoint(checkpoint); }, "inverted interval accepted");
    checkpoint = sample();
    checkpoint.ordering.push_back(1);
    require_rejected([&] { cutwidth::validate_checkpoint(checkpoint); }, "duplicate vertex accepted");
    checkpoint = sample();
    checkpoint.completed_thresholds.push_back(
        {6, cutwidth::CompletedThresholdResult::infeasible});
    require_rejected([&] { cutwidth::validate_checkpoint(checkpoint); }, "duplicate threshold accepted");
    checkpoint = sample();
    std::swap(checkpoint.completed_thresholds[0], checkpoint.completed_thresholds[1]);
    require_rejected([&] { cutwidth::validate_checkpoint(checkpoint); },
                     "unsorted thresholds accepted");
    checkpoint = sample();
    checkpoint.upper_bound = 9;
    require_rejected([&] { cutwidth::validate_checkpoint(checkpoint); },
                     "contradictory feasible proof accepted");
    checkpoint = sample();
    checkpoint.lower_bound = 6;
    require_rejected([&] { cutwidth::validate_checkpoint(checkpoint); },
                     "contradictory infeasible proof accepted");
    checkpoint = sample();
    checkpoint.graph_hash.clear();
    require_rejected([&] { cutwidth::validate_checkpoint(checkpoint); }, "empty hash accepted");
}

void atomic_file_test() {
    const auto directory = std::filesystem::temp_directory_path() / "cutwidth-checkpoint-tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "state.cwcp";
    const auto first = sample();
    cutwidth::write_checkpoint_atomic(path, first);
    require(cutwidth::read_checkpoint(path) == first, "written checkpoint differs");
    auto second = first;
    second.elapsed_milliseconds++;
    second.interrupted = true;
    cutwidth::write_checkpoint_atomic(path, second);
    require(cutwidth::read_checkpoint(path) == second, "atomic replacement retained old state");
    auto invalid = second;
    invalid.lower_bound = invalid.upper_bound + 1;
    require_rejected([&] { cutwidth::write_checkpoint_atomic(path, invalid); },
                     "invalid checkpoint was written");
    require(cutwidth::read_checkpoint(path) == second,
            "failed checkpoint write damaged prior valid state");
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        require(entry.path() == path, "temporary checkpoint was left behind");
    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "CWCP1\n";
    }
    require_rejected([&] { (void)cutwidth::read_checkpoint(path); },
                     "corrupt checkpoint file accepted");
    std::filesystem::remove_all(directory);
}

void adaptive_checkpoint_test() {
    const cutwidth::Graph graph(6, {{0,1},{0,3},{1,2},{1,4},{2,3},{2,5},{3,4},{4,5}});
    cutwidth::DecisionSession session(graph, 2);
    (void)session.service({3, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    if (session.status() == cutwidth::SessionStatus::unresolved)
        (void)session.donate_unexplored_sibling();
    cutwidth::AdaptiveCheckpoint checkpoint;
    checkpoint.graph_hash = "sha256:graph";
    checkpoint.solver_semantic_hash = "sha256:solver";
    checkpoint.proof_policy_hash = "sha256:proof";
    checkpoint.candidate_order_hash = "sha256:order";
    checkpoint.vertex_count = 6;
    checkpoint.declared_memory_bytes = 1024 * 1024;
    checkpoint.ordering = {0,1,2,3,4,5};
    checkpoint.lower_bound = 1;
    checkpoint.upper_bound = 10;
    checkpoint.elapsed_milliseconds = 77;
    cutwidth::sdp::ProgressiveSdpSnapshot sdp_snapshot;
    sdp_snapshot.live_generations = {{2, 41}};
    cutwidth::sdp::ProgressiveSdpTask sdp_task;
    sdp_task.id = {2, 41, {0}, 1};
    sdp_task.accumulated_subtree_nodes = 3;
    sdp_task.existing_certified_bound = 1;
    sdp_task.root = true;
    sdp_snapshot.tasks.push_back(std::move(sdp_task));
    sdp_snapshot.cursor = 0;
    sdp_snapshot.committed_records.push_back({{2, 41, {0}, 1}, 2,
        "certificate", "graph", "model", "backend"});
    sdp_snapshot.certified_lower_bound = 2;
    checkpoint.progressive_sdp = std::move(sdp_snapshot);
    cutwidth::ProgressiveCheapBoundSnapshot cheap_snapshot;
    cheap_snapshot.live_generations = {{2, 41}};
    cheap_snapshot.tasks.push_back({{2, 41, 7, {0, 3}}});
    checkpoint.progressive_cheap_bounds = std::move(cheap_snapshot);
    auto residual_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    const std::vector<cutwidth::Graph::Mask> residual_prefix(graph.word_count(), 0);
    cutwidth::ResidualDpSession residual(graph, residual_prefix, residual_governor);
    require(residual.applicable(), "CWCP2 residual-DP fixture was not applicable");
    (void)residual.service(5);
    checkpoint.residual_dp = residual.snapshot();
    if (session.status() == cutwidth::SessionStatus::unresolved)
        checkpoint.sessions.push_back(session.quiesce_and_snapshot());
    const cutwidth::Graph parallel_graph(6,
        {{0,1},{0,3},{0,5},{1,2},{1,4},{2,3},{2,5},{3,4},{4,5}});
    cutwidth::DecisionOptions parallel_options;
    parallel_options.failed_state_cache_memory_bytes = 1U << 20;
    cutwidth::ParallelDecisionSession parallel(parallel_graph, 2, parallel_options, 4);
    (void)parallel.service({2,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)});
    if (parallel.status() == cutwidth::SessionStatus::unresolved)
        checkpoint.parallel_sessions.push_back(parallel.quiesce_and_snapshot());
    const auto directory = std::filesystem::temp_directory_path() / "cutwidth-checkpoint-v2-tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "state.cwcp2";
    cutwidth::write_adaptive_checkpoint_atomic(path, checkpoint);
    const auto restored = cutwidth::read_adaptive_checkpoint(path);
    require(restored.graph_hash == checkpoint.graph_hash &&
            restored.solver_semantic_hash == checkpoint.solver_semantic_hash &&
            restored.proof_policy_hash == checkpoint.proof_policy_hash &&
            restored.candidate_order_hash == checkpoint.candidate_order_hash &&
            restored.lower_bound == checkpoint.lower_bound &&
            restored.upper_bound == checkpoint.upper_bound &&
            restored.sessions.size() == checkpoint.sessions.size() &&
            restored.parallel_sessions.size() == checkpoint.parallel_sessions.size() &&
            restored.progressive_sdp && restored.progressive_cheap_bounds &&
            restored.progressive_sdp->live_generations == checkpoint.progressive_sdp->live_generations &&
            restored.progressive_sdp->tasks.size() == 1 &&
            restored.progressive_sdp->tasks.front().id == checkpoint.progressive_sdp->tasks.front().id &&
            restored.progressive_sdp->committed_records.size() == 1 &&
            restored.progressive_sdp->certified_lower_bound == checkpoint.progressive_sdp->certified_lower_bound &&
            restored.progressive_cheap_bounds->live_generations == checkpoint.progressive_cheap_bounds->live_generations &&
            restored.progressive_cheap_bounds->tasks.size() == 1 &&
            restored.progressive_cheap_bounds->tasks.front().id == checkpoint.progressive_cheap_bounds->tasks.front().id &&
            restored.residual_dp &&
            restored.residual_dp->initial_prefix == checkpoint.residual_dp->initial_prefix &&
            restored.residual_dp->remaining == checkpoint.residual_dp->remaining &&
            restored.residual_dp->projection->states == checkpoint.residual_dp->projection->states &&
            restored.residual_dp->table == checkpoint.residual_dp->table &&
            restored.residual_dp->next_state == checkpoint.residual_dp->next_state,
            "CWCP2 round trip changed ledger state");
    auto restored_residual_governor = std::make_shared<cutwidth::MemoryGovernor>(1U << 20, 0);
    auto restored_residual = cutwidth::ResidualDpSession::restore(
        graph, *restored.residual_dp, restored_residual_governor);
    require(restored_residual.applicable(), "CWCP2 residual-DP snapshot was not restorable");
    if (!restored.sessions.empty()) {
        cutwidth::DecisionSession resumed(graph, restored.sessions.front());
        while (resumed.status() == cutwidth::SessionStatus::unresolved)
            (void)resumed.service({5, std::chrono::steady_clock::now() + std::chrono::seconds(1)});
        require(resumed.status() != cutwidth::SessionStatus::cancelled,
                "CWCP2 restored session could not finish");
    }
    if (!restored.parallel_sessions.empty()) {
        cutwidth::ParallelDecisionSession resumed(
            parallel_graph, restored.parallel_sessions.front(), parallel_options, 4);
        std::uint64_t calls = 0;
        while (resumed.status() == cutwidth::SessionStatus::unresolved) {
            (void)resumed.service({8,
                std::chrono::steady_clock::now() + std::chrono::seconds(1)});
            require(++calls < 100000, "CWCP2 restored parallel forest stalled");
        }
        require(resumed.status() != cutwidth::SessionStatus::cancelled,
                "CWCP2 restored parallel forest could not finish");
    }
    cutwidth::validate_checkpoint_compatibility(restored,
        {checkpoint.graph_hash, checkpoint.solver_semantic_hash,
         checkpoint.proof_policy_hash, checkpoint.candidate_order_hash});
    require_rejected([&] {
        cutwidth::validate_checkpoint_compatibility(restored,
            {"different", checkpoint.solver_semantic_hash,
             checkpoint.proof_policy_hash, checkpoint.candidate_order_hash});
    }, "CWCP2 graph mismatch accepted");
    auto bad_regions = restored;
    if (!bad_regions.sessions.empty()) {
        ++bad_regions.sessions.front().unfinished_regions;
        require_rejected([&] { cutwidth::validate_adaptive_checkpoint(bad_regions); },
                         "CWCP2 inconsistent region accounting accepted");
    }
    auto bad_residual = restored;
    bad_residual.residual_dp->table.pop_back();
    require_rejected([&] { cutwidth::validate_adaptive_checkpoint(bad_residual); },
                     "CWCP2 malformed residual-DP table accepted");
    {
        std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekp(20);
        const char byte = 'X';
        corrupt.write(&byte, 1);
    }
    require_rejected([&] { (void)cutwidth::read_adaptive_checkpoint(path); },
                     "CWCP2 digest tampering accepted");
    std::filesystem::remove_all(directory);
}

void shutdown_checkpoint_policy_test() {
    const cutwidth::Graph graph(6, {{0,1},{0,3},{1,2},{1,4},{2,3},{2,5},{3,4},{4,5}});
    const auto directory = std::filesystem::temp_directory_path() / "cutwidth-shutdown-checkpoint-tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto cp_path = directory / "state.cwcp2";

    // Cache omission is an explicit snapshot contract, independent of how
    // quickly the adaptive scheduler promotes a threshold to parallel DFS.
    {
        cutwidth::DecisionOptions decision_options;
        decision_options.cache_mode = cutwidth::CacheMode::fixed_threshold;
        decision_options.failed_state_cache_memory_bytes = 1U << 20;
        cutwidth::ParallelDecisionSession parallel(graph, 2, decision_options, 2);
        const auto warm_snapshot = parallel.quiesce_and_snapshot();
        require(warm_snapshot.fixed_cache.has_value(),
                "parallel fixture did not create its acceleration cache");
        const auto shutdown_snapshot = parallel.quiesce_and_snapshot(
            cutwidth::SnapshotPolicy::omit_cache);
        require(!shutdown_snapshot.fixed_cache.has_value(),
                "shutdown snapshot copied the acceleration cache");
    }

    // 1. Solved run writes no checkpoint
    {
        cutwidth::OptimizerV2Options options;
        options.controller = cutwidth::ControllerMode::adaptive;
        options.proof_backend = cutwidth::ProofBackend::dfs;
        options.time_limit = std::chrono::seconds{10};
        options.checkpoint_out = cp_path;

        const auto result = cutwidth::optimize_cutwidth_v2(graph, options);
        require(result.optimal, "small graph was not solved optimally");
        require(!std::filesystem::exists(cp_path), "solved run wrote a checkpoint");
    }

    // 2. Unresolved timed-out run writes one resumable checkpoint
    {
        constexpr std::uint32_t side = 15;
        std::vector<std::pair<cutwidth::Graph::Vertex, cutwidth::Graph::Vertex>> edges;
        for (std::uint32_t row = 0; row < side; ++row) {
            for (std::uint32_t column = 0; column < side; ++column) {
                const auto vertex = row * side + column;
                if (row + 1 < side) edges.emplace_back(vertex, vertex + side);
                if (column + 1 < side) edges.emplace_back(vertex, vertex + 1);
            }
        }
        const cutwidth::Graph grid15(side * side, std::move(edges));

        cutwidth::OptimizerV2Options options;
        options.controller = cutwidth::ControllerMode::adaptive;
        options.proof_backend = cutwidth::ProofBackend::dfs;
        options.threads = 2;
        options.time_limit = std::chrono::milliseconds{100};
        options.checkpoint_out = cp_path;
        options.use_twin_symmetry = false;

        std::filesystem::remove(cp_path);

        const auto result = cutwidth::optimize_cutwidth_v2(grid15, options);
        require(!result.optimal, "Grid15 unexpectedly solved inside the timeout fixture");
        require(result.stats.checkpoint_reserve_milliseconds == 0,
                "timeout checkpoint invented a hidden deadline reserve");
        require(std::filesystem::exists(cp_path), "unresolved timed-out run did not write a checkpoint");

        const auto restored = cutwidth::read_adaptive_checkpoint(cp_path);
        require(restored.lower_bound == result.lower_bound, "restored lower bound differs");
        require(restored.upper_bound == result.upper_bound, "restored upper bound differs");

        // 3. Checkpoint parallel snapshots have no fixed cache
        require(!restored.sessions.empty() || !restored.parallel_sessions.empty(),
                "timeout checkpoint did not preserve any live DFS session");
        for (const auto& session : restored.parallel_sessions) {
            require(!session.fixed_cache.has_value(), "parallel session snapshot contains a fixed cache");
        }

        // Resume only needs to prove continuation compatibility here. Requiring
        // a deliberately hard timeout fixture to become optimal would make the
        // checkpoint policy test machine-speed dependent.
        auto resume_options = options;
        resume_options.time_limit = std::chrono::milliseconds{1};
        resume_options.checkpoint_out.reset();
        resume_options.resume = cp_path;

        const auto resumed_result = cutwidth::optimize_cutwidth_v2(grid15, resume_options);
        require(resumed_result.stats.resumed_from_checkpoint,
                "resume run did not report restored continuation state");
        require(resumed_result.lower_bound >= restored.lower_bound,
                "resume weakened the certified lower bound");
        require(resumed_result.upper_bound <= restored.upper_bound,
                "resume weakened the incumbent upper bound");
    }

    // 4. No positive time limit writes no checkpoint
    {
        std::filesystem::remove(cp_path);

        cutwidth::OptimizerV2Options options;
        options.controller = cutwidth::ControllerMode::adaptive;
        options.proof_backend = cutwidth::ProofBackend::dfs;
        options.time_limit = std::chrono::milliseconds{0};
        options.checkpoint_out = cp_path;

        const auto result = cutwidth::optimize_cutwidth_v2(graph, options);
        require(result.optimal, "small graph was not solved optimally");
        require(!std::filesystem::exists(cp_path), "no positive time limit wrote a checkpoint");
    }

    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    try {
        round_trip_test();
        corruption_tests();
        invariant_tests();
        atomic_file_test();
        adaptive_checkpoint_test();
        shutdown_checkpoint_policy_test();
        std::cout << "All checkpoint tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CHECKPOINT TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
