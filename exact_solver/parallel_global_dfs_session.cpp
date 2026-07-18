#include "parallel_global_dfs_session.hpp"

#include <stdexcept>

namespace cutwidth {

ParallelGlobalDFSSession::ParallelGlobalDFSSession(
    std::shared_ptr<ParallelDecisionSession> session, std::uint64_t grant_work_units)
    : session_(std::move(session)), grant_work_units_(grant_work_units) {
    if (!session_) throw std::invalid_argument("parallel global DFS adapter needs a session");
    if (grant_work_units_ == 0) throw std::invalid_argument("global DFS grant must be nonzero");
    threshold_ = 0; // Bound by prepare(), before executor dispatches a lease.
}

void ParallelGlobalDFSSession::set_grant_work_units(std::uint64_t grant_work_units) {
    if (grant_work_units == 0)
        throw std::invalid_argument("global DFS grant must be nonzero");
    std::lock_guard lock(mutex_);
    if (epoch_open_)
        throw std::logic_error("cannot change an active global DFS grant");
    grant_work_units_ = grant_work_units;
}

void ParallelGlobalDFSSession::prepare(std::uint64_t generation, std::uint32_t threshold) {
    std::lock_guard lock(mutex_);
    if (epoch_open_) throw std::logic_error("cannot replace an active global DFS grant");
    if (session_->status() != SessionStatus::unresolved)
        throw std::logic_error("cannot prepare a terminal parallel session");
    threshold_ = threshold;
    generation_ = generation;
    prepared_ = true;
    revoked_.store(false);
}

void ParallelGlobalDFSSession::open_epoch(const EpochContract& contract) {
    std::lock_guard lock(mutex_);
    if (epoch_open_) throw std::logic_error("cannot replace an active global DFS grant");
    if (session_->status() != SessionStatus::unresolved)
        throw std::logic_error("cannot prepare a terminal parallel session");
    threshold_ = contract.threshold;
    generation_ = contract.generation;
    prepared_ = true;
    revoked_.store(false);

    std::uint64_t work_units = contract.total_work_units > 0 ? contract.total_work_units : grant_work_units_;

    EpochContract resolved_contract = contract;
    resolved_contract.total_work_units = work_units;

    session_->begin_external_epoch(resolved_contract);
    epoch_open_ = true;
}

LeaseOutcome ParallelGlobalDFSSession::run_one_lease(
    std::size_t worker_id, std::chrono::steady_clock::time_point deadline) {
    {
        std::lock_guard lock(mutex_);
        if (!prepared_ || revoked_.load() || session_->status() != SessionStatus::unresolved) {
            LeaseOutcome outcome;
            outcome.status = LeaseOutcome::terminal;
            return outcome;
        }
        if (!epoch_open_) {
            throw std::logic_error("lease dispatched to unopened epoch");
        }
    }
    return session_->run_external_lease(worker_id);
}

void ParallelGlobalDFSSession::quiesce() {
    std::lock_guard lock(mutex_);
    if (!epoch_open_) return;
    // finish waits for every in-flight lease, then reconciles the forest.
    last_event_ = session_->finish_external_epoch();
    epoch_open_ = false;
}

ParallelSessionEvent ParallelGlobalDFSSession::last_event() const {
    std::lock_guard lock(mutex_);
    return last_event_;
}

bool ParallelGlobalDFSSession::has_work() const {
    std::lock_guard lock(mutex_);
    return prepared_ && !revoked_.load() && session_->status() == SessionStatus::unresolved;
}

bool ParallelGlobalDFSSession::has_runnable_work() const {
    std::lock_guard lock(mutex_);
    return prepared_ && !revoked_.load() && session_->has_runnable_work();
}

void ParallelGlobalDFSSession::increment_steal_reservation() {
    session_->increment_steal_reservation();
}

void ParallelGlobalDFSSession::decrement_steal_reservation() {
    session_->decrement_steal_reservation();
}

void ParallelGlobalDFSSession::revoke() {
    revoked_.store(true);
    session_->request_yield();
}

} // namespace cutwidth
