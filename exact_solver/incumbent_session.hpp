#pragma once

#include "graph.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace cutwidth {

struct IncumbentSessionStats {
    std::uint64_t service_calls = 0;
    std::uint64_t iterations = 0;
    std::uint64_t candidate_evaluations = 0;
    std::uint64_t accepted_moves = 0;
    std::uint64_t verified_improvements = 0;
    std::uint64_t no_progress_bursts = 0;
    double service_seconds = 0.0;
    bool operator==(const IncumbentSessionStats&) const = default;
};

// Cache-free continuation state for a persistent incumbent trajectory.  This
// deliberately includes the currently open destroy/repair fragment so a
// checkpoint does not restart a potentially expensive repair round.
struct IncumbentRepairSnapshot {
    std::vector<Graph::Vertex> kept;
    std::vector<Graph::Vertex> pending;
    Graph::Vertex vertex = 0;
    std::size_t next_position = 0;
    std::vector<Graph::Vertex> best;
    std::vector<std::uint32_t> best_profile;
    std::uint32_t best_width = std::numeric_limits<std::uint32_t>::max();
    bool operator==(const IncumbentRepairSnapshot&) const = default;
};

struct IncumbentSnapshot {
    std::vector<Graph::Vertex> current_ordering;
    std::vector<Graph::Vertex> best_ordering;
    std::uint32_t current_width = 0;
    std::uint32_t best_width = 0;
    std::uint64_t rng_state = 0;
    std::size_t operator_cursor = 0;
    std::size_t destroy_scale = 1;
    std::optional<IncumbentRepairSnapshot> repair;
    IncumbentSessionStats stats;
    bool operator==(const IncumbentSnapshot&) const = default;
};

struct IncumbentServiceResult {
    bool improved = false;
    bool deadline_reached = false;
    std::uint32_t width = 0;
    std::vector<Graph::Vertex> ordering;
    std::uint64_t iterations_completed = 0;
    std::uint64_t work_units_completed = 0;
};

// A graph-local, resumable ALNS trajectory. Every ordering exposed by this
// class is a verified permutation; only a strict width improvement is reported
// to callers as an incumbent change.
class PersistentIncumbentSession {
public:
    PersistentIncumbentSession(const Graph& graph,
                               std::vector<Graph::Vertex> initial_ordering);
    PersistentIncumbentSession(const Graph& graph, const IncumbentSnapshot& snapshot);

    [[nodiscard]] IncumbentServiceResult service(
        std::uint64_t maximum_work_units,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max());

    [[nodiscard]] std::uint32_t best_width() const noexcept { return best_width_; }
    [[nodiscard]] const std::vector<Graph::Vertex>& best_ordering() const noexcept {
        return best_ordering_;
    }
    [[nodiscard]] const IncumbentSessionStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t rng_state() const noexcept { return rng_; }
    [[nodiscard]] std::size_t destroy_scale() const noexcept { return destroy_scale_; }
    [[nodiscard]] IncumbentSnapshot snapshot() const;

private:
    [[nodiscard]] std::uint64_t next_random() noexcept;
    [[nodiscard]] std::vector<std::size_t> choose_destroy_positions();
    struct RepairState {
        std::vector<Graph::Vertex> kept;
        std::vector<Graph::Vertex> pending;
        Graph::Vertex vertex = 0;
        std::size_t next_position = 0;
        std::vector<Graph::Vertex> best;
        std::vector<std::uint32_t> best_profile;
        std::uint32_t best_width = std::numeric_limits<std::uint32_t>::max();
    };
    [[nodiscard]] static RepairState restore_repair(
        const IncumbentRepairSnapshot& snapshot);
    [[nodiscard]] IncumbentRepairSnapshot snapshot_repair(
        const RepairState& state) const;
    void validate_snapshot(const IncumbentSnapshot& snapshot) const;
    void begin_repair();
    // Completes one best-insertion candidate evaluation. Partial repair state
    // remains live across controller yields.
    [[nodiscard]] bool repair_step();
    void finish_repaired_candidate(std::vector<Graph::Vertex> candidate);
    [[nodiscard]] std::vector<std::uint32_t> cut_profile(
        const std::vector<Graph::Vertex>& ordering) const;
    void advance_policy() noexcept;

    const Graph& graph_;
    std::vector<Graph::Vertex> current_ordering_;
    std::vector<Graph::Vertex> best_ordering_;
    std::uint32_t current_width_ = 0;
    std::uint32_t best_width_ = 0;
    std::uint64_t rng_ = 0;
    std::size_t operator_cursor_ = 0;
    std::size_t destroy_scale_ = 1;
    std::optional<RepairState> repair_;
    IncumbentSessionStats stats_;
};

} // namespace cutwidth
