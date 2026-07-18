#pragma once

#include "parallel_decision_session.hpp"
#include "partial_bounds.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace cutwidth {

struct ProgressiveCheapBoundTaskId {
    std::uint32_t threshold = 0;
    std::uint64_t generation = 0;
    std::uint64_t region_id = 0;
    std::vector<Graph::Vertex> prefix;
    friend bool operator==(const ProgressiveCheapBoundTaskId&,
                           const ProgressiveCheapBoundTaskId&) = default;
};

struct ProgressiveCheapBoundTask { ProgressiveCheapBoundTaskId id; };

struct ProgressiveCheapBoundSnapshot {
    std::map<std::uint32_t, std::uint64_t> live_generations;
    std::vector<ProgressiveCheapBoundTask> tasks;
    std::size_t cursor = 0;
};

struct ProgressiveCheapBoundEvent {
    std::optional<ProgressiveCheapBoundTaskId> task;
    bool stale_rejected = false;
    bool fragment_rejected = false;
    bool certified_prune = false;
    PartialBoundStats stats;

    std::uint32_t lagrangian_best_bound = 0;
    std::uint32_t lagrangian_best_cardinality = 0;
    int64_t lagrangian_best_numerator = 0;
    int64_t lagrangian_best_denominator = 1;
    bool lagrangian_evaluated = false;
};

// Logical controller queue for cheap certified partial bounds.  Tasks are
// admitted solely from a ParallelDecisionSession's never-started forest
// records; retirement remains owned by that exact forest.
class ProgressiveCheapBoundSession {
public:
    ProgressiveCheapBoundSession(const Graph& graph, PartialBoundOptions options)
        : graph_(graph), options_(options) {}
    void activate_threshold(std::uint32_t threshold, std::uint64_t generation);
    void deactivate_threshold(std::uint32_t threshold);
    [[nodiscard]] bool enqueue(const ParallelUnstartedFragment& fragment,
                               std::uint64_t generation);
    // Evaluate a previously claimed fragment without touching its forest.
    // This is safe to run through the shared executor while DFS works on the
    // remaining forest.  The caller must subsequently commit_or_release.
    [[nodiscard]] ProgressiveCheapBoundEvent evaluate_claimed_one(
        const ParallelUnstartedFragment& fragment);
    [[nodiscard]] bool commit_or_release_claimed(
        ParallelDecisionSession& forest, const ParallelUnstartedFragment& fragment,
        ProgressiveCheapBoundEvent& event);
    [[nodiscard]] ProgressiveCheapBoundEvent service_one(ParallelDecisionSession& forest);
    [[nodiscard]] bool has_pending() const noexcept { return cursor_ < tasks_.size(); }
    [[nodiscard]] ProgressiveCheapBoundSnapshot snapshot() const;
    void restore(const ProgressiveCheapBoundSnapshot& snapshot);

private:
    [[nodiscard]] bool live(const ProgressiveCheapBoundTaskId& id) const noexcept;
    const Graph& graph_;
    PartialBoundOptions options_;
    std::map<std::uint32_t, std::uint64_t> live_generations_;
    std::vector<ProgressiveCheapBoundTask> tasks_;
    std::size_t cursor_ = 0;
};

} // namespace cutwidth
