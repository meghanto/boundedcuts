#pragma once

#include "decision.hpp"
#include "vertex_set.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace cutwidth {

enum class SessionStatus { unresolved, feasible, infeasible, cancelled };
enum class SessionYieldReason {
    quantum_complete,
    yield_requested,
    worker_donation,
    ownership_wait,
    memory_pressure,
    interval_resolved,
    deadline,
    exception,
    terminal,
};

struct SessionServiceBudget {
    // A controller-calibrated unit.  The initial engine uses one expanded node
    // as a unit; the controller owns calibration and never changes correctness.
    std::uint64_t work_units = 1;
    std::chrono::steady_clock::time_point absolute_deadline =
        std::chrono::steady_clock::time_point::max();
};

struct SessionServiceEvent {
    std::uint32_t threshold = 0;
    SessionYieldReason reason = SessionYieldReason::quantum_complete;
    SessionStatus status = SessionStatus::unresolved;
    DecisionStats delta;
    std::uint64_t safe_points = 0;
    bool right_censored = true;
};

struct SessionCandidateSnapshot {
    Graph::Vertex vertex = 0;
    std::uint32_t cut = 0;
    bool operator==(const SessionCandidateSnapshot&) const = default;
};

struct SessionFrameSnapshot {
    std::uint32_t cut = 0;
    Graph::Vertex incoming = 0;
    bool has_incoming = false;
    bool entered = false;
    std::vector<SessionCandidateSnapshot> candidates;
    std::size_t next_candidate = 0;
    bool operator==(const SessionFrameSnapshot&) const = default;
};

struct SessionPendingSnapshot {
    std::vector<Graph::Vertex> path;
    std::uint32_t cut = 0;
    bool operator==(const SessionPendingSnapshot&) const = default;
};

// Proof-relevant, cache-free state. Hash tables and advisory ownership are
// deliberately rebuilt empty after resume.
struct SessionSnapshot {
    std::uint32_t threshold = 0;
    SessionStatus status = SessionStatus::unresolved;
    std::vector<Graph::Vertex> path;
    std::vector<Graph::Vertex> ordering;
    std::vector<SessionFrameSnapshot> frames;
    std::vector<SessionPendingSnapshot> pending;
    std::uint64_t unfinished_regions = 0;
    // Regions owned by child continuations in a parallel region forest.
    // Zero for ordinary standalone snapshots.
    std::uint64_t external_regions = 0;
    bool continuation_partitioned = false;
    std::uint64_t controller_quantum = 1;
    std::uint64_t controller_services = 0;
    std::uint64_t session_generation = 0;
    DecisionStats stats;
};

// A persistent exact threshold decision.  A quantum end is unresolved, never
// an infeasibility proof.  The explicit stack is retained across service calls.
class DecisionSession {
public:
    using SafePointHook = std::function<void(std::uint64_t)>;

    DecisionSession(const Graph& graph, std::uint32_t threshold,
                    DecisionOptions options = {});
    DecisionSession(const Graph& graph, const SessionSnapshot& snapshot,
                    DecisionOptions options = {});
    ~DecisionSession();
    DecisionSession(DecisionSession&&) noexcept;
    DecisionSession& operator=(DecisionSession&&) noexcept;
    DecisionSession(const DecisionSession&) = delete;
    DecisionSession& operator=(const DecisionSession&) = delete;

    [[nodiscard]] SessionServiceEvent service(const SessionServiceBudget& budget);
    // Detaches the deepest unstarted sibling into a separate pending region.
    // The call is made only while the session is quiescent; the asynchronous
    // controller converts a live request into this operation at a safe point.
    [[nodiscard]] bool donate_unexplored_sibling();
    [[nodiscard]] std::optional<SessionSnapshot> extract_pending_continuation();
    void resolve_external_failure();
    // Used only by the owning parallel proof forest after a separately
    // certified bound rejects an as-yet-unstarted continuation.
    void mark_certified_infeasible();
    void publish_external_witness(const std::vector<Graph::Vertex>& ordering);
    void request_yield() noexcept;
    void cancel() noexcept;
    [[nodiscard]] SessionStatus status() const noexcept;
    [[nodiscard]] std::uint32_t threshold() const noexcept;
    [[nodiscard]] const std::vector<Graph::Vertex>& ordering() const noexcept;
    [[nodiscard]] const DecisionStats& stats() const noexcept;
    [[nodiscard]] std::size_t stack_depth() const noexcept;
    [[nodiscard]] std::uint64_t unfinished_regions() const noexcept;
    [[nodiscard]] std::size_t pending_continuations() const noexcept;
    [[nodiscard]] SessionSnapshot quiesce_and_snapshot() const;
    [[nodiscard]] SessionSnapshot quiesce_region_snapshot() const;
    [[nodiscard]] bool waiting_for_external_regions() const noexcept;

    // Test-only fault-injection hook. It observes each safe point but cannot
    // mutate proof state directly.
    void set_safe_point_hook(SafePointHook hook);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cutwidth
