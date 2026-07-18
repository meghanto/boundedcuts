#include "progressive_sdp_session.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace cutwidth::sdp {

SdpBoundResult SdpBoundOracleAdapter::bound(const SdpBoundRequest& request) {
    return oracle_.bound(request);
}

bool operator<(const ProgressiveSdpTaskId& a, const ProgressiveSdpTaskId& b) noexcept {
    return std::tie(a.threshold, a.generation, a.cardinality, a.prefix) <
           std::tie(b.threshold, b.generation, b.cardinality, b.prefix);
}

void ProgressiveSdpSession::activate_threshold(std::uint32_t threshold,
                                               std::uint64_t generation) {
    live_generations_[threshold] = generation;
}

void ProgressiveSdpSession::deactivate_threshold(std::uint32_t threshold) {
    live_generations_.erase(threshold);
}

bool ProgressiveSdpSession::enqueue(ProgressiveSdpTask task) {
    if (!is_live(task.id)) return false;
    tasks_.push_back(std::move(task));
    sort_pending();
    return true;
}

ProgressiveSdpServiceEvent ProgressiveSdpSession::service_one(
    std::chrono::steady_clock::time_point deadline) {
    ProgressiveSdpServiceEvent event;
    if (!has_pending()) return event;

    const ProgressiveSdpTask& task = tasks_[cursor_];
    event.task = task.id;
    if (!is_live(task.id)) {
        event.stale_rejected = true;
        ++cursor_; // Stale identities are terminally rejected, not retried.
        return event;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        // Do not consume a task that never entered the oracle. This is what
        // makes a checkpoint/resume trajectory preserve queued work exactly.
        event.deadline_rejected = true;
        return event;
    }

    SdpBoundRequest request;
    request.prefix = task.id.prefix;
    request.cardinality = task.id.cardinality;
    request.accumulated_subtree_nodes = task.accumulated_subtree_nodes;
    request.existing_certified_bound = task.existing_certified_bound;
    request.root = task.root;
    request.caller_deadline = deadline;
    running_task_ = cursor_;
    SdpBoundResult result;
    try {
        result = oracle_.bound(request);
    } catch (...) {
        running_task_.reset();
        throw;
    }
    running_task_.reset();
    ++cursor_;
    event.status = result.status;
    if (result.status != SdpBoundStatus::certified || !result.certified_lower_bound) {
        return event;
    }

    const std::uint32_t lower_bound = *result.certified_lower_bound;
    committed_records_.push_back({task.id, lower_bound, result.proof_kind,
                                  result.graph_hash, result.model_hash,
                                  result.backend_hash});
    if (!certified_lower_bound_ || lower_bound > *certified_lower_bound_) {
        certified_lower_bound_ = lower_bound;
    }
    event.committed = true;
    return event;
}

ProgressiveSdpSnapshot ProgressiveSdpSession::snapshot() const {
    return {live_generations_, tasks_, cursor_, committed_records_, certified_lower_bound_};
}

void ProgressiveSdpSession::restore(const ProgressiveSdpSnapshot& snapshot) {
    if (snapshot.cursor > snapshot.tasks.size()) {
        throw std::invalid_argument("invalid progressive SDP snapshot cursor");
    }
    live_generations_ = snapshot.live_generations;
    tasks_ = snapshot.tasks;
    cursor_ = snapshot.cursor;
    committed_records_ = snapshot.committed_records;
    certified_lower_bound_ = snapshot.certified_lower_bound;
    running_task_.reset();
    sort_pending();
}

bool ProgressiveSdpSession::is_live(const ProgressiveSdpTaskId& id) const noexcept {
    const auto it = live_generations_.find(id.threshold);
    return it != live_generations_.end() && it->second == id.generation;
}

void ProgressiveSdpSession::sort_pending() {
    std::sort(tasks_.begin() + static_cast<std::ptrdiff_t>(cursor_), tasks_.end(),
              [](const ProgressiveSdpTask& a, const ProgressiveSdpTask& b) {
                  return a.id < b.id;
              });
}

} // namespace cutwidth::sdp
