#pragma once

#include "decision_session.hpp"
#include "decision_cache.hpp"
#include "global_dfs_executor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace cutwidth {

enum class SnapshotPolicy {
    include_cache,
    omit_cache
};

struct ParallelSessionEvent {
    SessionStatus status = SessionStatus::unresolved;
    SessionYieldReason reason = SessionYieldReason::quantum_complete;
    DecisionStats delta;
    std::size_t workers_used = 0;
    std::uint64_t donations = 0;
    std::uint64_t terminal_regions = 0;
    double busy_worker_seconds = 0.0;
    double allocated_worker_seconds = 0.0;
    bool right_censored = true;
    std::uint64_t configured_proof_regions_bound = 0;
    std::uint64_t resolved_proof_regions_bound = 0;
    std::uint64_t peak_proof_regions = 0;
    std::uint64_t suppressed_donations = 0;
};

struct ParallelRegionSnapshot {
    std::uint64_t region_id = 0;
    std::uint64_t parent_region_id = 0;
    SessionSnapshot session;
};

struct ParallelDecisionSnapshot {
    std::uint32_t threshold = 0;
    SessionStatus status = SessionStatus::unresolved;
    std::vector<Graph::Vertex> ordering;
    std::vector<ParallelRegionSnapshot> regions;
    std::uint64_t controller_quantum = 1;
    std::uint64_t controller_services = 0;
    std::uint64_t session_generation = 0;
    // Fixed-threshold caches are proof-local.  Persist one physical sharded
    // image for the entire forest, never a copy per donated region.
    std::optional<ShardedFixedThresholdDynamicCacheSnapshot> fixed_cache;
};

// An immutable identity for a forest continuation that has never entered DFS.
// It is intentionally not a general partial-state export: only donated or
// bootstrap proof-forest records may be retired by a certified cheap bound.
struct ParallelUnstartedFragment {
    std::uint64_t region_id = 0;
    // Zero denotes the legacy, inspect-only identity. A nonzero value is a
    // short-lived reservation nonce returned only by claim_unstarted_fragment.
    // It prevents an old claimant from retiring a later re-claim of the same
    // otherwise identical proof state.
    std::uint64_t reservation_id = 0;
    SessionSnapshot session;
};

// A persistent fixed worker pool. Every worker exclusively owns a continuation
// while it is running; idle capacity is filled by deepest-sibling donation.
// In external-worker mode the session owns only its proof forest.  A
// GlobalDFSExecutor supplies worker ids and never overlaps two leases of the
// same forest.  The default remains the historical owned-pool behaviour.
class ParallelDecisionSession {
public:
    ParallelDecisionSession(const Graph& graph, std::uint32_t threshold,
                            DecisionOptions options, std::size_t workers,
                            bool external_workers = false);
    ParallelDecisionSession(const Graph& graph,
                            const ParallelDecisionSnapshot& snapshot,
                            DecisionOptions options, std::size_t workers,
                            bool external_workers = false);
    ~ParallelDecisionSession();
    ParallelDecisionSession(const ParallelDecisionSession&) = delete;
    ParallelDecisionSession& operator=(const ParallelDecisionSession&) = delete;

    [[nodiscard]] ParallelSessionEvent service(const SessionServiceBudget& budget);
    // Ask an active service epoch to return at its next calibrated control
    // poll without cancelling the exact session or changing its proof status.
    void request_yield() noexcept;
    void cancel() noexcept;
    [[nodiscard]] SessionStatus status() const noexcept;
    [[nodiscard]] std::vector<Graph::Vertex> ordering() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] ParallelDecisionSnapshot quiesce_and_snapshot(
        SnapshotPolicy policy = SnapshotPolicy::include_cache) const;
    [[nodiscard]] std::optional<ParallelUnstartedFragment>
    inspect_unstarted_fragment() const;
    [[nodiscard]] std::optional<ParallelUnstartedFragment>
    inspect_deepest_unstarted_fragment() const;
    // Atomically reserves one never-started region. Reserved work is removed
    // from DFS admission until the claimant either retires it or releases it.
    // Claims are quiescent control operations; snapshots reject active claims.
    [[nodiscard]] std::optional<ParallelUnstartedFragment>
    claim_unstarted_fragment();
    [[nodiscard]] std::optional<ParallelUnstartedFragment>
    claim_deepest_unstarted_fragment();
    [[nodiscard]] std::optional<ParallelUnstartedFragment>
    donate_and_claim_deepest_unstarted_fragment();
    // Atomically validates the region id and prefix before retiring it as an
    // exact failure. Returns false for stale, started, or mismatched work.
    [[nodiscard]] bool retire_unstarted_fragment(
        const ParallelUnstartedFragment& fragment);
    // Claim-only variants require the exact nonzero reservation identity
    // returned by claim_unstarted_fragment.
    [[nodiscard]] bool retire_claimed_unstarted_fragment(
        const ParallelUnstartedFragment& fragment);
    [[nodiscard]] bool release_claimed_unstarted_fragment(
        const ParallelUnstartedFragment& fragment);

    // Valid only for external-worker sessions. begin/finish are control
    // boundaries; leases may run concurrently in between on distinct ids.
    void begin_external_epoch(std::uint64_t work_units,
                              std::chrono::steady_clock::time_point deadline);
    void begin_external_epoch(const EpochContract& contract);
    [[nodiscard]] LeaseOutcome run_external_lease(std::size_t worker_id);
    [[nodiscard]] ParallelSessionEvent finish_external_epoch();
    [[nodiscard]] bool has_runnable_work() const;
    void increment_steal_reservation();
    void decrement_steal_reservation();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cutwidth
