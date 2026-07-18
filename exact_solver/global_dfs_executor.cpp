#include "global_dfs_executor.hpp"

#include <algorithm>
#include <stdexcept>

namespace cutwidth {

GlobalDFSExecutor::GlobalDFSExecutor(std::size_t workers,
                                     std::chrono::milliseconds lease_quantum)
    : lease_quantum_(lease_quantum), workers_count_(workers), worker_ids_in_use_(workers, false), pool_(workers) {
    if (workers == 0 || lease_quantum.count() <= 0)
        throw std::invalid_argument("global DFS executor needs workers and a positive quantum");
}

GlobalDFSExecutor::~GlobalDFSExecutor() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
}

void GlobalDFSExecutor::register_session(std::shared_ptr<GlobalWorkSession> session) {
    if (!session || !session->binding_identity())
        throw std::invalid_argument("cannot register global work session without an identity");
    std::lock_guard lock(mutex_);
    for (const auto& existing : sessions_) {
        if (existing == session) return;
        if (existing->binding_identity() == session->binding_identity())
            throw std::invalid_argument("global work binding identity is already registered");
    }
    sessions_.push_back(std::move(session));
}

void GlobalDFSExecutor::unregister_session(const std::shared_ptr<GlobalWorkSession>& session) {
    if (!session) return;
    // Unregistration is a self-contained maintenance operation.  It must not
    // leave the controller pool paused after removing an obsolete threshold;
    // otherwise the next granted epoch has queued permits but no runnable
    // workers and drain_epoch waits forever.
    quiesce_all();
    try {
        std::lock_guard lock(mutex_);
        for (const auto& [id, epoch] : epochs_)
            if (epoch.key == session->binding_identity())
                throw std::logic_error("drain a session's epochs before unregistering it");
        // This is permanent retirement.  Checkpoint quiescence below is not:
        // it only pauses admission so a live forest remains resumable.
        session->revoke();
        bindings_.erase(session->binding_identity());
        std::erase(sessions_, session);
    } catch (...) {
        resume();
        throw;
    }
    resume();
}

std::uint64_t GlobalDFSExecutor::bind_locked(
    const std::shared_ptr<GlobalWorkSession>& session, std::uint32_t threshold) {
    const auto key = session->binding_identity();
    if (!key) throw std::invalid_argument("global work session has no binding identity");
    const auto existing = bindings_.find(key);
    if (existing != bindings_.end()) {
        if (existing->second.threshold != threshold)
            throw std::invalid_argument("persistent global DFS session changed threshold");
        return existing->second.generation;
    }
    if (std::find(sessions_.begin(), sessions_.end(), session) == sessions_.end())
        throw std::invalid_argument("grant requires a registered session");
    const auto generation = ++generations_[threshold];
    bindings_.emplace(key, Binding{session, threshold, generation});
    session->prepare(generation, threshold);
    return generation;
}

std::uint64_t GlobalDFSExecutor::bind_session(
    const std::shared_ptr<GlobalWorkSession>& session, std::uint32_t threshold) {
    std::lock_guard lock(mutex_);
    return bind_locked(session, threshold);
}

std::uint64_t GlobalDFSExecutor::grant_epoch(
    const std::shared_ptr<GlobalWorkSession>& session, std::uint32_t threshold,
    std::size_t permits) {
    EpochAdmitRequest req;
    req.session = session;
    req.threshold = threshold;
    req.permits = permits;
    req.work_units = 0;
    req.deadline = std::chrono::steady_clock::time_point::max();
    auto ids = grant_epochs({req});
    return ids.front();
}

std::vector<std::uint64_t> GlobalDFSExecutor::grant_epochs(
    const std::vector<EpochAdmitRequest>& requests) {
    std::lock_guard lock(mutex_);

    std::vector<std::uint64_t> epoch_ids;
    epoch_ids.reserve(requests.size());

    std::vector<std::pair<std::shared_ptr<GlobalWorkSession>, EpochContract>> opened_epochs;
    opened_epochs.reserve(requests.size());

    for (const auto& req : requests) {
        if (req.permits == 0) throw std::invalid_argument("global DFS epoch needs a permit");

        const auto generation = bind_locked(req.session, req.threshold);
        for (const auto& [id, epoch] : epochs_) {
            if (epoch.key == req.session->binding_identity() && !epoch.draining) {
                throw std::logic_error("session already has a granted global epoch");
            }
        }

        const auto id = next_epoch_++;
        epoch_ids.push_back(id);

        EpochContract contract;
        contract.epoch_id = id;
        contract.threshold = req.threshold;
        contract.generation = generation;
        contract.concurrency_target = req.permits;
        contract.total_work_units = req.work_units;
        contract.deadline = std::min(req.deadline, overall_deadline_);

        epochs_.emplace(id, Epoch{id, req.session->binding_identity(), req.permits, 0, false});

        opened_epochs.emplace_back(req.session, contract);
    }

    // Now, open all epochs BEFORE submitting any Dispenso tasks
    for (auto& pair : opened_epochs) {
        pair.first->open_epoch(pair.second);
    }

    // Now enqueue permits and submit dispenso tasks
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto id = epoch_ids[i];
        const auto permits = requests[i].permits;
        enqueue_permits_locked(id, permits);

        if (!paused_) {
            for (std::size_t j = 0; j < permits; ++j) {
                submit_dispenso_task_locked();
            }
        }
    }

    return epoch_ids;
}

void GlobalDFSExecutor::drain_epoch(std::uint64_t epoch_id) {
    std::shared_ptr<GlobalWorkSession> session;
    {
        std::unique_lock lock(mutex_);
        const auto it = epochs_.find(epoch_id);
        if (it == epochs_.end()) throw std::invalid_argument("unknown global DFS epoch");
        it->second.draining = true;
        epoch_cv_.wait(lock, [&] { return it->second.pending == 0 && it->second.active == 0; });
        const auto binding = bindings_.find(it->second.key);
        if (binding == bindings_.end()) throw std::logic_error("epoch lost its binding");
        session = binding->second.session;
        epochs_.erase(it);
    }
    session->quiesce();
}

void GlobalDFSExecutor::drain_epochs(const std::vector<std::uint64_t>& epoch_ids) {
    std::vector<std::shared_ptr<GlobalWorkSession>> sessions;
    sessions.reserve(epoch_ids.size());
    {
        std::unique_lock lock(mutex_);
        for (auto epoch_id : epoch_ids) {
            if (epochs_.find(epoch_id) == epochs_.end()) {
                throw std::invalid_argument("unknown global DFS epoch");
            }
        }
        for (auto epoch_id : epoch_ids) {
            epochs_.at(epoch_id).draining = true;
        }
        for (auto epoch_id : epoch_ids) {
            auto it = epochs_.find(epoch_id);
            if (it == epochs_.end()) throw std::logic_error("epoch disappeared during drain");
            epoch_cv_.wait(lock, [&] { return it->second.pending == 0 && it->second.active == 0; });
            const auto binding = bindings_.find(it->second.key);
            if (binding == bindings_.end()) throw std::logic_error("epoch lost its binding");
            sessions.push_back(binding->second.session);
            epochs_.erase(it);
        }
    }
    for (auto& session : sessions) {
        session->quiesce();
    }
}


void GlobalDFSExecutor::enqueue_permits_locked(std::uint64_t epoch_id,
                                                std::size_t permits) {
    // A newly admitted arm receives recurring service before an earlier epoch
    // consumes the entire queue. To avoid priority inversion/starvation, we reconstruct
    // the queue by round-robin scheduling all pending permits in epoch ID (chronological) order.
    std::unordered_map<std::uint64_t, std::size_t> counts;
    for (auto id : permits_) {
        counts[id]++;
    }
    counts[epoch_id] += permits;

    std::vector<std::uint64_t> active_ids;
    for (const auto& [id, count] : counts) {
        if (count > 0) {
            active_ids.push_back(id);
        }
    }
    std::sort(active_ids.begin(), active_ids.end());

    std::deque<std::uint64_t> next_permits;
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto id : active_ids) {
            if (counts[id] > 0) {
                next_permits.push_back(id);
                counts[id]--;
                progress = true;
            }
        }
    }
    permits_.swap(next_permits);
}

void GlobalDFSExecutor::quiesce_all() {
    {
        std::lock_guard lock(mutex_);
        paused_ = true;
    }
    std::unique_lock lock(mutex_);
    epoch_cv_.wait(lock, [&] { return active_leases_ == 0; });
}

void GlobalDFSExecutor::resume() {
    std::lock_guard lock(mutex_);
    paused_ = false;
    for (std::size_t i = 0; i < permits_.size(); ++i) {
        submit_dispenso_task_locked();
    }
}

std::size_t GlobalDFSExecutor::active_leases_count() const { std::lock_guard lock(mutex_); return active_leases_; }
std::uint64_t GlobalDFSExecutor::generation(std::uint32_t threshold) const { std::lock_guard lock(mutex_); const auto it=generations_.find(threshold); return it==generations_.end()?0:it->second; }
double GlobalDFSExecutor::busy_worker_seconds() const { std::lock_guard lock(mutex_); return busy_seconds_; }
double GlobalDFSExecutor::allocated_worker_seconds() const { std::lock_guard lock(mutex_); return allocated_seconds_; }

void GlobalDFSExecutor::submit_dispenso_task_locked() {
    pool_.schedule([this]() {
        std::uint64_t epoch_id = 0;
        std::shared_ptr<GlobalWorkSession> session;
        std::size_t worker_id = std::numeric_limits<std::size_t>::max();

        {
            std::unique_lock lock(mutex_);
            if (stopping_ || paused_ || permits_.empty()) {
                return;
            }

            epoch_id = permits_.front();
            permits_.pop_front();

            auto it = epochs_.find(epoch_id);
            if (it == epochs_.end()) {
                return;
            }
            auto& epoch = it->second;
            --epoch.pending;
            ++epoch.active;
            ++active_leases_;
            if (active_leases_ > telemetry_.peak_active_leases) {
                telemetry_.peak_active_leases = active_leases_;
            }

            session = bindings_.at(epoch.key).session;

            for (std::size_t i = 0; i < worker_ids_in_use_.size(); ++i) {
                if (!worker_ids_in_use_[i]) {
                    worker_ids_in_use_[i] = true;
                    worker_id = i;
                    break;
                }
            }
            if (worker_id == std::numeric_limits<std::size_t>::max()) {
                worker_id = 0;
            }
        }

        const auto started = std::chrono::steady_clock::now();
        auto lease_deadline = started + lease_quantum_;
        {
            std::lock_guard lock(mutex_);
            if (overall_deadline_ < lease_deadline) {
                lease_deadline = overall_deadline_;
            }
        }

        auto outcome = session->run_one_lease(worker_id, lease_deadline);
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        std::uint64_t current_epoch_id = epoch_id;
        std::shared_ptr<GlobalWorkSession> current_session = session;
        auto handoff_deadline = lease_deadline;

        {
            std::lock_guard lock(mutex_);
            allocated_seconds_ += elapsed;
            if (outcome.consumed_work_units != 0 || outcome.busy_seconds > 0.0) {
                busy_seconds_ += outcome.busy_seconds;
                telemetry_.useful_leases++;
                telemetry_.per_epoch_useful_work[current_epoch_id] += outcome.consumed_work_units;
                const auto bind_it = bindings_.find(epochs_.at(current_epoch_id).key);
                if (bind_it != bindings_.end()) {
                    telemetry_.per_threshold_useful_work[bind_it->second.threshold] += outcome.consumed_work_units;
                }
            }
        }
        if (outcome.status == LeaseOutcome::useful &&
            !current_session->has_runnable_work()) {
            outcome.status = LeaseOutcome::terminal;
        }

        // Steal loop: when a physical worker receives an empty/no-runnable outcome, reassign to another epoch
        std::size_t steal_attempts = 0;
        bool outcome_counted = false;
        while (outcome.status == LeaseOutcome::empty ||
               outcome.status == LeaseOutcome::no_runnable ||
               outcome.status == LeaseOutcome::terminal) {
            if (steal_attempts >= workers_count_ * 2) {
                break;
            }
            if (!outcome_counted && outcome.status != LeaseOutcome::terminal) {
                std::lock_guard lock(mutex_);
                telemetry_.empty_claim_exits++;
            }
            outcome_counted = true;

            std::uint64_t target_epoch_id = 0;
            std::shared_ptr<GlobalWorkSession> target_session;

            {
                std::lock_guard lock(mutex_);
                struct Candidate {
                    std::uint64_t epoch_id;
                    std::uint32_t threshold;
                    std::shared_ptr<GlobalWorkSession> session;
                };
                std::vector<Candidate> candidates;
                for (const auto& [id, ep] : epochs_) {
                    // drain_epoch() is a join, not a cancellation request. A
                    // draining epoch remains eligible to receive an already
                    // active physical worker until its bounded work is done.
                    if (id == current_epoch_id) continue;

                    const auto bind_it = bindings_.find(ep.key);
                    if (bind_it == bindings_.end()) continue;
                    auto s = bind_it->second.session;
                    if (s && s->has_runnable_work()) {
                        candidates.push_back(Candidate{id, bind_it->second.threshold, s});
                    }
                }

                if (!candidates.empty()) {
                    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                        if (a.threshold != b.threshold) return a.threshold < b.threshold;
                        return a.epoch_id < b.epoch_id;
                    });

                    const auto& chosen = candidates.front();
                    target_epoch_id = chosen.epoch_id;
                    target_session = chosen.session;

                    // Transfer this already-active physical slot. Requiring an
                    // unclaimed queued permit made stealing impossible after
                    // the window's Dispenso tasks had all started.
                    auto& curr_ep = epochs_.at(current_epoch_id);
                    if (curr_ep.active == 0)
                        throw std::logic_error("steal donor lost its active lease");
                    --curr_ep.active;

                    auto& tgt_ep = epochs_.at(target_epoch_id);
                    ++tgt_ep.active;
                    telemetry_.cross_session_steals++;
                }
            }

            if (target_epoch_id == 0) {
                // A proof owner may be between the safe point that detached a
                // sibling and publication of that lightweight record. Keep
                // this physical lease available for the remainder of its
                // bounded handoff quantum, but do not spin or sleep on any one
                // proof forest.
                const auto now = std::chrono::steady_clock::now();
                if (now >= handoff_deadline) break;
                const auto remaining = handoff_deadline - now;
                std::this_thread::sleep_for(std::min(
                    remaining,
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::microseconds(50))));
                continue;
            }

            steal_attempts++;

            const auto started_stolen = std::chrono::steady_clock::now();
            auto stolen_lease_deadline = started_stolen + lease_quantum_;
            {
                std::lock_guard lock(mutex_);
                if (overall_deadline_ < stolen_lease_deadline) {
                    stolen_lease_deadline = overall_deadline_;
                }
            }

            // Expose reservation/steal intent on the recipient session
            target_session->increment_steal_reservation();

            outcome = target_session->run_one_lease(worker_id, stolen_lease_deadline);

            target_session->decrement_steal_reservation();

            const auto elapsed_stolen = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_stolen).count();

            {
                std::lock_guard lock(mutex_);
                allocated_seconds_ += elapsed_stolen;
                if (outcome.consumed_work_units != 0 || outcome.busy_seconds > 0.0) {
                    busy_seconds_ += outcome.busy_seconds;
                    telemetry_.useful_leases++;
                    telemetry_.per_epoch_useful_work[target_epoch_id] += outcome.consumed_work_units;
                    const auto bind_it = bindings_.find(epochs_.at(target_epoch_id).key);
                    if (bind_it != bindings_.end()) {
                        telemetry_.per_threshold_useful_work[bind_it->second.threshold] += outcome.consumed_work_units;
                    }
                }
            }

            current_epoch_id = target_epoch_id;
            current_session = target_session;
            handoff_deadline = stolen_lease_deadline;
            outcome_counted = false;
            if (outcome.status == LeaseOutcome::useful &&
                !current_session->has_runnable_work()) {
                outcome.status = LeaseOutcome::terminal;
            }
        }

        {
            std::lock_guard lock(mutex_);
            auto it = epochs_.find(current_epoch_id);
            if (it != epochs_.end()) {
                auto& epoch = it->second;
                --epoch.active;
            }
            --active_leases_;

            if (worker_id < worker_ids_in_use_.size()) {
                worker_ids_in_use_[worker_id] = false;
            }

            epoch_cv_.notify_all();
        }
    });
}

void GlobalDFSExecutor::set_overall_deadline(std::chrono::steady_clock::time_point deadline) {
    std::lock_guard lock(mutex_);
    overall_deadline_ = deadline;
}

} // namespace cutwidth
