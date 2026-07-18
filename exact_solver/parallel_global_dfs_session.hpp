#pragma once

#include "global_dfs_executor.hpp"
#include "parallel_decision_session.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace cutwidth {

// Bridges one persistent proof forest to the global worker pool.  A grant is
// opened lazily by its first lease and drained exactly once by quiesce().
class ParallelGlobalDFSSession final : public GlobalDFSSession {
public:
    ParallelGlobalDFSSession(std::shared_ptr<ParallelDecisionSession> session,
                             std::uint64_t grant_work_units);
    // The controller owns work entitlement; executor permits only select how
    // many workers may consume that entitlement concurrently.
    void set_grant_work_units(std::uint64_t grant_work_units);
    void prepare(std::uint64_t generation_identity, std::uint32_t threshold) override;
    void open_epoch(const EpochContract& contract) override;
    LeaseOutcome run_one_lease(std::size_t worker_id,
                               std::chrono::steady_clock::time_point deadline) override;
    void quiesce() override;
    [[nodiscard]] bool has_work() const override;
    [[nodiscard]] bool has_runnable_work() const override;
    void increment_steal_reservation() override;
    void decrement_steal_reservation() override;
    [[nodiscard]] std::uint32_t threshold() const override { return threshold_; }
    [[nodiscard]] std::uint64_t generation() const override { return generation_; }
    void revoke() override;
    [[nodiscard]] bool is_revoked() const override { return revoked_.load(); }
    [[nodiscard]] double busy_worker_seconds() const override { return 0.0; }
    [[nodiscard]] double allocated_worker_seconds() const override { return 0.0; }
    // Valid after the executor drains this session's epoch.  The event is
    // retained here because GlobalDFSSession::quiesce deliberately has no
    // return value.
    [[nodiscard]] ParallelSessionEvent last_event() const;

private:
    std::shared_ptr<ParallelDecisionSession> session_;
    std::uint64_t grant_work_units_;
    mutable std::mutex mutex_;
    std::uint32_t threshold_ = 0;
    std::uint64_t generation_ = 0;
    bool prepared_ = false;
    bool epoch_open_ = false;
    ParallelSessionEvent last_event_;
    std::atomic<bool> revoked_{false};
};

} // namespace cutwidth
