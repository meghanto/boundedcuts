#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <dispenso/thread_pool.h>

namespace cutwidth {

struct EpochContract {
    std::uint64_t epoch_id;
    std::uint32_t threshold;
    std::uint64_t generation;
    std::size_t concurrency_target;
    std::uint64_t total_work_units;
    std::chrono::steady_clock::time_point deadline;
};

struct LeaseOutcome {
    enum Status {
        useful,
        empty,
        no_runnable,
        terminal
    } status = empty;
    std::uint64_t consumed_work_units = 0;
    std::uint64_t nodes_expanded = 0;
    double busy_seconds = 0.0;
};

// Neutral unit of controller work admitted through the shared exact-search
// pool.  The opaque identity is deliberately separate from the object address:
// an adapter may be reconstructed while preserving its durable session key.
class GlobalWorkSession {
public:
    virtual ~GlobalWorkSession() = default;
    [[nodiscard]] virtual const void* binding_identity() const noexcept { return this; }
    virtual void prepare(std::uint64_t generation, std::uint32_t threshold) = 0;
    virtual void open_epoch(const EpochContract& contract) { (void)contract; }
    virtual LeaseOutcome run_one_lease(std::size_t worker_id,
                                       std::chrono::steady_clock::time_point deadline) = 0;
    virtual void quiesce() = 0;
    virtual bool has_work() const = 0;
    [[nodiscard]] virtual bool has_runnable_work() const = 0;
    virtual std::uint32_t threshold() const = 0;
    virtual std::uint64_t generation() const = 0;
    virtual void revoke() = 0;
    virtual bool is_revoked() const = 0;
    virtual double busy_worker_seconds() const = 0;
    virtual double allocated_worker_seconds() const = 0;
    virtual void increment_steal_reservation() {}
    virtual void decrement_steal_reservation() {}
};

// Kept as a distinct DFS-facing type so current proof-forest clients retain
// their source-level contract while auxiliary controller arms use the neutral
// interface above.
class GlobalDFSSession : public GlobalWorkSession {
public:
    ~GlobalDFSSession() override = default;
};

// A controller-owned worker pool. Registration establishes identity only;
// workers run exclusively from explicitly granted epoch permits.
class GlobalDFSExecutor {
public:
    explicit GlobalDFSExecutor(std::size_t workers,
        std::chrono::milliseconds lease_quantum = std::chrono::milliseconds(10));
    ~GlobalDFSExecutor();
    GlobalDFSExecutor(const GlobalDFSExecutor&) = delete;
    GlobalDFSExecutor& operator=(const GlobalDFSExecutor&) = delete;

    void register_session(std::shared_ptr<GlobalWorkSession> session);
    void unregister_session(const std::shared_ptr<GlobalWorkSession>& session);

    // Bind once per persistent session. Rebinding the same session/threshold
    // retains its generation, which is essential for K=15 continuity.
    std::uint64_t bind_session(const std::shared_ptr<GlobalWorkSession>& session,
                               std::uint32_t threshold);

    struct EpochAdmitRequest {
        std::shared_ptr<GlobalWorkSession> session;
        std::uint32_t threshold;
        std::size_t permits;
        std::uint64_t work_units;
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    };

    // Grant a bounded epoch. No lease may run before this call.
    std::uint64_t grant_epoch(const std::shared_ptr<GlobalWorkSession>& session,
                              std::uint32_t threshold, std::size_t permits);
    // Grant multiple bounded epochs atomically in a single batch.
    std::vector<std::uint64_t> grant_epochs(const std::vector<EpochAdmitRequest>& requests);
    // Wait for its permits and leases, then close the session's epoch exactly once.
    void drain_epoch(std::uint64_t epoch_id);
    // Wait for all permits and leases of multiple epochs, then close their epochs in order.
    void drain_epochs(const std::vector<std::uint64_t>& epoch_ids);

    // Stops admission and safely returns all executing leases; queued permits
    // remain durable until their owning epoch is drained or resumed.
    void quiesce_all();
    void resume();
    void set_overall_deadline(std::chrono::steady_clock::time_point deadline);

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_count_; }
    [[nodiscard]] std::size_t active_leases_count() const;
    [[nodiscard]] std::uint64_t generation(std::uint32_t threshold) const;
    [[nodiscard]] double busy_worker_seconds() const;
    [[nodiscard]] double allocated_worker_seconds() const;

    struct Telemetry {
        std::size_t peak_active_leases = 0;
        std::uint64_t useful_leases = 0;
        std::uint64_t empty_claim_exits = 0;
        std::uint64_t cross_session_steals = 0;
        std::unordered_map<std::uint64_t, std::uint64_t> per_epoch_useful_work;
        std::unordered_map<std::uint32_t, std::uint64_t> per_threshold_useful_work;
    };

    [[nodiscard]] Telemetry get_telemetry() const {
        std::lock_guard lock(mutex_);
        return telemetry_;
    }

private:
    using BindingIdentity = const void*;
    struct Binding { std::shared_ptr<GlobalWorkSession> session; std::uint32_t threshold; std::uint64_t generation; };
    struct Epoch { std::uint64_t id; BindingIdentity key; std::size_t pending = 0; std::size_t active = 0; bool draining = false; };

    void submit_dispenso_task_locked();
    [[nodiscard]] std::uint64_t bind_locked(const std::shared_ptr<GlobalWorkSession>&,
                                            std::uint32_t);
    void enqueue_permits_locked(std::uint64_t epoch_id, std::size_t permits);

    const std::chrono::milliseconds lease_quantum_;
    mutable std::mutex mutex_;
    std::condition_variable epoch_cv_;
    bool stopping_ = false, paused_ = false;
    std::size_t workers_count_;
    std::vector<bool> worker_ids_in_use_;
    dispenso::ThreadPool pool_;
    std::vector<std::shared_ptr<GlobalWorkSession>> sessions_;
    std::unordered_map<BindingIdentity, Binding> bindings_;
    std::unordered_map<std::uint32_t, std::uint64_t> generations_;
    std::unordered_map<std::uint64_t, Epoch> epochs_;
    std::deque<std::uint64_t> permits_;
    std::uint64_t next_epoch_ = 1;
    std::size_t active_leases_ = 0;
    double busy_seconds_ = 0.0, allocated_seconds_ = 0.0;
    std::chrono::steady_clock::time_point overall_deadline_ = std::chrono::steady_clock::time_point::max();
    Telemetry telemetry_;
};

} // namespace cutwidth
