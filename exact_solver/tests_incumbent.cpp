#include "graph.hpp"
#include "incumbent_session.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_same_trajectory(const cutwidth::PersistentIncumbentSession& lhs,
                             const cutwidth::PersistentIncumbentSession& rhs) {
    const auto left = lhs.snapshot();
    const auto right = rhs.snapshot();
    require(left.current_ordering == right.current_ordering &&
                left.best_ordering == right.best_ordering &&
                left.current_width == right.current_width &&
                left.best_width == right.best_width &&
                left.rng_state == right.rng_state &&
                left.operator_cursor == right.operator_cursor &&
                left.destroy_scale == right.destroy_scale &&
                left.repair == right.repair &&
                left.stats.service_calls == right.stats.service_calls &&
                left.stats.iterations == right.stats.iterations &&
                left.stats.candidate_evaluations == right.stats.candidate_evaluations &&
                left.stats.accepted_moves == right.stats.accepted_moves &&
                left.stats.verified_improvements == right.stats.verified_improvements &&
                left.stats.no_progress_bursts == right.stats.no_progress_bursts,
            "restored incumbent diverged from its fixed-work trajectory");
}
}

int main() {
    try {
        const cutwidth::Graph graph(8, {
            {0,1},{0,3},{0,6},{1,2},{1,5},{2,3},{2,7},
            {3,4},{4,5},{4,7},{5,6},{6,7}});
        const std::vector<cutwidth::Graph::Vertex> seed{0,1,2,3,4,5,6,7};
        cutwidth::PersistentIncumbentSession whole(graph, seed);
        cutwidth::PersistentIncumbentSession split(graph, seed);
        const auto a = whole.service(12);
        (void)split.service(5);
        const auto b = split.service(7);
        require(a.width == b.width && a.ordering == b.ordering,
                "split service did not resume the same ALNS trajectory");
        require(whole.rng_state() == split.rng_state() &&
                whole.destroy_scale() == split.destroy_scale(),
                "persistent policy state changed across burst boundaries");
        require(graph.validate_ordering(a.ordering) &&
                graph.ordering_cutwidth(a.ordering) == a.width,
                "session exposed an unverified incumbent");
        require(a.width <= graph.ordering_cutwidth(seed),
                "session worsened the best incumbent");

        cutwidth::PersistentIncumbentSession interrupted(graph, seed);
        (void)interrupted.service(1);
        const auto mid_repair = interrupted.snapshot();
        require(mid_repair.repair.has_value(),
                "incumbent snapshot did not retain mid-repair state");
        cutwidth::PersistentIncumbentSession restored(graph, mid_repair);
        require(restored.snapshot() == mid_repair,
                "incumbent snapshot did not exactly restore policy and statistics");
        for (std::uint64_t work : {1U, 2U, 1U, 4U, 3U, 6U}) {
            const auto uninterrupted_result = interrupted.service(work);
            const auto restored_result = restored.service(work);
            require(uninterrupted_result.improved == restored_result.improved &&
                        uninterrupted_result.width == restored_result.width &&
                        uninterrupted_result.ordering == restored_result.ordering &&
                        uninterrupted_result.iterations_completed ==
                            restored_result.iterations_completed &&
                        uninterrupted_result.work_units_completed ==
                            restored_result.work_units_completed,
                    "incumbent snapshot changed service result");
            require_same_trajectory(interrupted, restored);
        }
        auto malformed_snapshot = mid_repair;
        ++malformed_snapshot.current_width;
        bool snapshot_rejected = false;
        try {
            cutwidth::PersistentIncumbentSession malformed(graph, malformed_snapshot);
        } catch (const std::invalid_argument&) { snapshot_rejected = true; }
        require(snapshot_rejected, "invalid incumbent snapshot accepted");

        const auto before = split.stats().iterations;
        const auto stopped = split.service(
            100, std::chrono::steady_clock::now() - std::chrono::milliseconds(1));
        require(stopped.deadline_reached && stopped.iterations_completed == 0 &&
                split.stats().iterations == before,
                "expired burst changed persistent search state");

        const cutwidth::Graph empty(0, {});
        cutwidth::PersistentIncumbentSession empty_session(
            empty, std::vector<cutwidth::Graph::Vertex>{});
        const auto empty_result = empty_session.service(3);
        require(empty_result.width == 0 && empty_result.ordering.empty(),
                "empty graph incumbent session failed");

        bool rejected = false;
        try {
            cutwidth::PersistentIncumbentSession invalid(graph, {0,1,2});
        } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid initial incumbent accepted");
        std::cout << "All persistent incumbent tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "INCUMBENT TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
