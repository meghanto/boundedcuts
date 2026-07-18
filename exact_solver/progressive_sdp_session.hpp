#pragma once

#include "graph.hpp"
#include "sdp_bound_oracle.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace cutwidth::sdp {

// The progressive scheduler deliberately knows no solver internals.  This is
// the narrow seam used both by SdpBoundOracle and deterministic test doubles.
class ProgressiveSdpOracle {
public:
    virtual ~ProgressiveSdpOracle() = default;
    virtual SdpBoundResult bound(const SdpBoundRequest& request) = 0;
};

class SdpBoundOracleAdapter final : public ProgressiveSdpOracle {
public:
    explicit SdpBoundOracleAdapter(SdpBoundOracle& oracle) : oracle_(oracle) {}
    SdpBoundResult bound(const SdpBoundRequest& request) override;

private:
    SdpBoundOracle& oracle_;
};

struct ProgressiveSdpTaskId {
    std::uint32_t threshold = 0;
    std::uint64_t generation = 0;
    std::vector<Graph::Mask> prefix;
    std::size_t cardinality = 0;

    friend bool operator==(const ProgressiveSdpTaskId&, const ProgressiveSdpTaskId&) = default;
    friend bool operator<(const ProgressiveSdpTaskId& a, const ProgressiveSdpTaskId& b) noexcept;
};

struct ProgressiveSdpTask {
    ProgressiveSdpTaskId id;
    std::uint64_t accumulated_subtree_nodes = 0;
    std::uint32_t existing_certified_bound = 0;
    bool root = false;
};

struct ProgressiveSdpRecord {
    ProgressiveSdpTaskId id;
    std::uint32_t certified_lower_bound = 0;
    std::string proof_kind;
    std::string graph_hash;
    std::string model_hash;
    std::string backend_hash;
};

struct ProgressiveSdpSnapshot {
    std::map<std::uint32_t, std::uint64_t> live_generations;
    std::vector<ProgressiveSdpTask> tasks;
    std::size_t cursor = 0;
    std::vector<ProgressiveSdpRecord> committed_records;
    std::optional<std::uint32_t> certified_lower_bound;
};

struct ProgressiveSdpServiceEvent {
    std::optional<ProgressiveSdpTaskId> task;
    std::optional<SdpBoundStatus> status;
    bool stale_rejected = false;
    bool deadline_rejected = false;
    bool committed = false;
};

// A deterministic, logical task queue.  It checkpoints only immutable task
// identities, the cursor, and already certified records; an interrupted SDP
// solve is simply retried from its queued task after resume.
class ProgressiveSdpSession {
public:
    explicit ProgressiveSdpSession(ProgressiveSdpOracle& oracle) : oracle_(oracle) {}

    void activate_threshold(std::uint32_t threshold, std::uint64_t generation);
    void deactivate_threshold(std::uint32_t threshold);
    [[nodiscard]] bool enqueue(ProgressiveSdpTask task);
    [[nodiscard]] ProgressiveSdpServiceEvent service_one(
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max());

    [[nodiscard]] bool has_pending() const noexcept { return cursor_ < tasks_.size(); }
    // A task is queued until service has admitted it to the oracle.  The
    // running marker is intentionally separate from checkpoint data: a
    // checkpoint never serializes an in-flight solve, so recovery retries it
    // as queued work.
    [[nodiscard]] bool has_running_task() const noexcept { return running_task_.has_value(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] const std::vector<ProgressiveSdpRecord>& committed_records() const noexcept {
        return committed_records_;
    }
    [[nodiscard]] const std::optional<std::uint32_t>& certified_lower_bound() const noexcept {
        return certified_lower_bound_;
    }
    [[nodiscard]] ProgressiveSdpSnapshot snapshot() const;
    void restore(const ProgressiveSdpSnapshot& snapshot);

private:
    [[nodiscard]] bool is_live(const ProgressiveSdpTaskId& id) const noexcept;
    void sort_pending();

    ProgressiveSdpOracle& oracle_;
    std::map<std::uint32_t, std::uint64_t> live_generations_;
    std::vector<ProgressiveSdpTask> tasks_;
    std::size_t cursor_ = 0;
    std::optional<std::size_t> running_task_;
    std::vector<ProgressiveSdpRecord> committed_records_;
    std::optional<std::uint32_t> certified_lower_bound_;
};

} // namespace cutwidth::sdp
