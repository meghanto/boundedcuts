#include "global_dfs_executor.hpp"
#include "parallel_global_dfs_session.hpp"
#include "oracle.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TestSession : public cutwidth::GlobalDFSSession {
public:
    explicit TestSession(std::string name) : name_(std::move(name)) {}

    void prepare(uint64_t generation_identity, uint32_t threshold) override {
        generation_ = generation_identity;
        threshold_ = threshold;
        revoked_ = false;
        has_work_ = true;
    }

    cutwidth::LeaseOutcome run_one_lease(std::size_t worker_id, std::chrono::steady_clock::time_point deadline) override {
        (void)worker_id;
        active_leases_++;
        lease_executions_++;

        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline && !revoked_) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        auto end = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();
        busy_seconds_ += elapsed;
        allocated_seconds_ += elapsed;

        active_leases_--;
        if (active_leases_ == 0) {
            std::lock_guard<std::mutex> lock(quiesce_mutex_);
            quiesce_cv_.notify_all();
        }
        cutwidth::LeaseOutcome outcome;
        outcome.status = cutwidth::LeaseOutcome::useful;
        outcome.consumed_work_units = 1;
        outcome.nodes_expanded = 1;
        return outcome;
    }
    bool has_runnable_work() const override { return has_work(); }

    void quiesce() override {
        revoke();
        std::unique_lock<std::mutex> lock(quiesce_mutex_);
        quiesce_cv_.wait(lock, [this]() { return active_leases_ == 0; });
    }

    bool has_work() const override {
        return has_work_;
    }

    void set_has_work(bool hw) {
        has_work_ = hw;
    }

    uint32_t threshold() const override {
        return threshold_;
    }

    uint64_t generation() const override {
        return generation_;
    }

    void revoke() override {
        revoked_ = true;
    }

    bool is_revoked() const override {
        return revoked_;
    }

    double busy_worker_seconds() const override {
        return busy_seconds_.load();
    }

    double allocated_worker_seconds() const override {
        return allocated_seconds_.load();
    }

    std::size_t get_active_leases() const {
        return active_leases_.load();
    }

    std::size_t get_lease_executions() const {
        return lease_executions_.load();
    }

    void reset_metrics() {
        lease_executions_ = 0;
        busy_seconds_ = 0.0;
        allocated_seconds_ = 0.0;
    }

private:
    std::string name_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint32_t> threshold_{0};
    std::atomic<bool> revoked_{false};
    std::atomic<bool> has_work_{false};
    std::atomic<std::size_t> active_leases_{0};
    std::atomic<std::size_t> lease_executions_{0};
    std::atomic<double> busy_seconds_{0.0};
    std::atomic<double> allocated_seconds_{0.0};
    std::mutex quiesce_mutex_;
    std::condition_variable quiesce_cv_;
};

struct LeaseRecorder {
    std::mutex mutex;
    std::vector<char> order;
    std::atomic<std::size_t> active{0};
    std::atomic<std::size_t> max_active{0};

    void enter(char label) {
        const auto now_active = active.fetch_add(1) + 1;
        auto seen = max_active.load();
        while (seen < now_active &&
               !max_active.compare_exchange_weak(seen, now_active)) {}
        std::lock_guard lock(mutex);
        order.push_back(label);
    }
    void leave() { active.fetch_sub(1); }
};

class RecordingDFSSession final : public cutwidth::GlobalDFSSession {
public:
    explicit RecordingDFSSession(LeaseRecorder& recorder) : recorder_(recorder) {}
    void prepare(uint64_t generation, uint32_t threshold) override {
        generation_ = generation; threshold_ = threshold; revoked_ = false;
    }
    cutwidth::LeaseOutcome run_one_lease(std::size_t,
                       std::chrono::steady_clock::time_point) override {
        recorder_.enter('D');
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        recorder_.leave();
        cutwidth::LeaseOutcome outcome;
        outcome.status = cutwidth::LeaseOutcome::useful;
        outcome.consumed_work_units = 1;
        outcome.nodes_expanded = 1;
        return outcome;
    }
    void quiesce() override {}
    bool has_work() const override { return !revoked_; }
    bool has_runnable_work() const override { return has_work(); }
    uint32_t threshold() const override { return threshold_; }
    uint64_t generation() const override { return generation_; }
    void revoke() override { revoked_ = true; }
    bool is_revoked() const override { return revoked_; }
    double busy_worker_seconds() const override { return 0.0; }
    double allocated_worker_seconds() const override { return 0.0; }
private:
    LeaseRecorder& recorder_;
    uint64_t generation_ = 0;
    uint32_t threshold_ = 0;
    bool revoked_ = false;
};

class RecordingAuxiliarySession final : public cutwidth::GlobalWorkSession {
public:
    explicit RecordingAuxiliarySession(LeaseRecorder& recorder) : recorder_(recorder) {}
    void prepare(uint64_t generation, uint32_t threshold) override {
        generation_ = generation; threshold_ = threshold; revoked_ = false;
    }
    cutwidth::LeaseOutcome run_one_lease(std::size_t,
                       std::chrono::steady_clock::time_point) override {
        recorder_.enter('A');
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        recorder_.leave();
        cutwidth::LeaseOutcome outcome;
        outcome.status = cutwidth::LeaseOutcome::useful;
        outcome.consumed_work_units = 1;
        outcome.nodes_expanded = 1;
        return outcome;
    }
    void quiesce() override {}
    bool has_work() const override { return !revoked_; }
    bool has_runnable_work() const override { return has_work(); }
    uint32_t threshold() const override { return threshold_; }
    uint64_t generation() const override { return generation_; }
    void revoke() override { revoked_ = true; }
    bool is_revoked() const override { return revoked_; }
    double busy_worker_seconds() const override { return 0.0; }
    double allocated_worker_seconds() const override { return 0.0; }
private:
    LeaseRecorder& recorder_;
    uint64_t generation_ = 0;
    uint32_t threshold_ = 0;
    bool revoked_ = false;
};

// Test 1: No work occurs before permits are granted (pre-grant idle)
void test_no_work_pre_grant() {
    cutwidth::GlobalDFSExecutor executor(2, std::chrono::milliseconds(10));

    auto session = std::make_shared<TestSession>("PreGrantSession");
    executor.register_session(session);

    // Let workers run idle for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify no leases were executed
    require(session->get_lease_executions() == 0, "No leases should execute when 0 permits granted");

    executor.unregister_session(session);
}

// Test 2: Capacity is bounded (never more leases than workers)
void test_capacity_bounded() {
    std::size_t num_workers = 3;
    cutwidth::GlobalDFSExecutor executor(num_workers, std::chrono::milliseconds(50));

    auto session = std::make_shared<TestSession>("CapacitySession");
    executor.register_session(session);

    // Grant more permits than workers to verify capacity constraint
    uint64_t epoch_id = executor.grant_epoch(session, 100, 20);

    std::size_t max_active = 0;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::size_t active = executor.active_leases_count();
        if (active > max_active) {
            max_active = active;
        }
        require(active <= num_workers, "Active leases " + std::to_string(active) + " exceeds worker count " + std::to_string(num_workers));
    }

    require(max_active > 0 && max_active <= num_workers, "At least 1 lease should run, bounded by worker count");

    executor.drain_epoch(epoch_id);
    executor.unregister_session(session);
}

// Test 3: Under-serviced granted session receives permits (fair scheduling)
void test_under_serviced_granted_session() {
    cutwidth::GlobalDFSExecutor executor(1, std::chrono::milliseconds(20));

    auto session_a = std::make_shared<TestSession>("SessionA");
    auto session_b = std::make_shared<TestSession>("SessionB");

    executor.register_session(session_a);
    executor.register_session(session_b);

    // Grant 5 permits to each
    uint64_t epoch_a = executor.grant_epoch(session_a, 100, 5);
    uint64_t epoch_b = executor.grant_epoch(session_b, 101, 5);

    // Let them run
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Both should have executed permits
    require(session_a->get_lease_executions() > 0, "Session A should have executed permits");
    require(session_b->get_lease_executions() > 0, "Session B should have executed permits");

    executor.drain_epoch(epoch_a);
    executor.drain_epoch(epoch_b);
    executor.unregister_session(session_a);
    executor.unregister_session(session_b);
}

// Test 4: No work occurs post-drain (epoch depleted, draining)
void test_no_work_post_drain() {
    cutwidth::GlobalDFSExecutor executor(2, std::chrono::milliseconds(10));

    auto session = std::make_shared<TestSession>("PostDrainSession");
    executor.register_session(session);

    // Grant a few permits
    uint64_t epoch_id = executor.grant_epoch(session, 100, 3);

    // Let them execute
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Drain the epoch
    executor.drain_epoch(epoch_id);

    // Reset and check no new work happens
    session->reset_metrics();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // No new executions should occur (epoch is drained)
    require(session->get_lease_executions() == 0, "No leases should execute post-drain");

    executor.unregister_session(session);
}

// Test 5: Permit identity preserved (threshold, generation, epoch)
void test_permit_identity_preserved() {
    cutwidth::GlobalDFSExecutor executor(1, std::chrono::milliseconds(20));

    auto session = std::make_shared<TestSession>("IdentitySession");
    executor.register_session(session);

    uint32_t threshold = 42;
    uint64_t epoch_id = executor.grant_epoch(session, threshold, 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Verify session's generation and threshold match
    require(session->generation() != 0, "Generation should be allocated");
    require(session->threshold() == threshold, "Threshold should match granted value");

    executor.drain_epoch(epoch_id);
    executor.unregister_session(session);
}

void test_real_parallel_forest_adapter() {
    const cutwidth::Graph graph(5, {{0,1},{1,2},{2,3},{3,4},{4,0},{0,2}});
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    options.use_failed_state_cache = true;
    options.failed_state_cache_memory_bytes = 1U << 20;
    auto forest = std::make_shared<cutwidth::ParallelDecisionSession>(
        graph, optimum, options, 2, true);
    auto adapter = std::make_shared<cutwidth::ParallelGlobalDFSSession>(forest, 256);
    cutwidth::GlobalDFSExecutor executor(2, std::chrono::milliseconds(2));
    executor.register_session(adapter);
    const auto epoch = executor.grant_epoch(adapter, optimum, 2);
    executor.drain_epoch(epoch);
    require(forest->status() == cutwidth::SessionStatus::feasible,
            "global executor adapter failed to finish a real feasible proof forest");
    const auto ordering = forest->ordering();
    require(graph.validate_ordering(ordering) && graph.ordering_cutwidth(ordering) <= optimum,
            "global executor adapter returned an invalid witness");
    executor.unregister_session(adapter);
}

void test_auxiliary_work_interleaves_with_dfs_under_shared_capacity() {
    LeaseRecorder recorder;
    cutwidth::GlobalDFSExecutor executor(1, std::chrono::milliseconds(10));
    auto dfs = std::make_shared<RecordingDFSSession>(recorder);
    auto auxiliary = std::make_shared<RecordingAuxiliarySession>(recorder);
    executor.register_session(dfs);
    executor.register_session(auxiliary);

    cutwidth::GlobalDFSExecutor::EpochAdmitRequest dfs_request;
    dfs_request.session = dfs;
    dfs_request.threshold = 15;
    dfs_request.permits = 4;
    cutwidth::GlobalDFSExecutor::EpochAdmitRequest auxiliary_request;
    auxiliary_request.session = auxiliary;
    auxiliary_request.threshold = 15;
    auxiliary_request.permits = 4;
    const auto epochs = executor.grant_epochs({dfs_request, auxiliary_request});
    executor.drain_epochs(epochs);

    require(recorder.order.size() == 8, "all DFS and auxiliary permits should run");
    for (std::size_t i = 1; i < recorder.order.size(); ++i)
        require(recorder.order[i] != recorder.order[i - 1],
                "queued DFS and auxiliary permits should interleave");
    require(recorder.max_active.load() <= executor.worker_count(),
            "shared auxiliary work exceeded executor worker capacity");

    executor.unregister_session(dfs);
    executor.unregister_session(auxiliary);
}

class BatchTestSession : public cutwidth::GlobalDFSSession {
public:
    BatchTestSession(std::string name, std::shared_ptr<bool> shared_failed, std::vector<std::shared_ptr<BatchTestSession>>& all_sessions)
        : name_(std::move(name)), shared_failed_(shared_failed), all_sessions_(all_sessions) {}

    void prepare(std::uint64_t generation, std::uint32_t threshold) override {
        generation_ = generation;
        threshold_ = threshold;
        prepared_ = true;
    }

    void open_epoch(const cutwidth::EpochContract& contract) override {
        std::lock_guard<std::mutex> lock(mutex_);
        opened_ = true;
        contract_ = contract;
    }

    cutwidth::LeaseOutcome run_one_lease(std::size_t worker_id, std::chrono::steady_clock::time_point deadline) override {
        (void)worker_id;
        (void)deadline;
        // Verify that ALL sessions in the batch are already opened
        for (const auto& s : all_sessions_) {
            if (!s->is_opened()) {
                *shared_failed_ = true;
            }
        }
        std::this_thread::yield();
        cutwidth::LeaseOutcome outcome;
        outcome.status = cutwidth::LeaseOutcome::useful;
        outcome.consumed_work_units = 1;
        outcome.nodes_expanded = 1;
        return outcome;
    }

    void quiesce() override {}
    bool has_work() const override { return prepared_ && !revoked_; }
    bool has_runnable_work() const override { return has_work(); }
    std::uint32_t threshold() const override { return threshold_; }
    std::uint64_t generation() const override { return generation_; }
    void revoke() override { revoked_ = true; }
    bool is_revoked() const override { return revoked_; }
    double busy_worker_seconds() const override { return 0.0; }
    double allocated_worker_seconds() const override { return 0.0; }

    bool is_opened() {
        std::lock_guard<std::mutex> lock(mutex_);
        return opened_;
    }

    cutwidth::EpochContract get_contract() {
        std::lock_guard<std::mutex> lock(mutex_);
        return contract_;
    }

private:
    std::string name_;
    std::shared_ptr<bool> shared_failed_;
    std::vector<std::shared_ptr<BatchTestSession>>& all_sessions_;
    std::uint64_t generation_ = 0;
    std::uint32_t threshold_ = 0;
    bool prepared_ = false;
    bool opened_ = false;
    bool revoked_ = false;
    std::mutex mutex_;
    cutwidth::EpochContract contract_;
};

void test_batched_admission_order() {
    cutwidth::GlobalDFSExecutor executor(2, std::chrono::milliseconds(10));
    auto failed = std::make_shared<bool>(false);
    std::vector<std::shared_ptr<BatchTestSession>> sessions;
    auto s1 = std::make_shared<BatchTestSession>("s1", failed, sessions);
    auto s2 = std::make_shared<BatchTestSession>("s2", failed, sessions);
    sessions.push_back(s1);
    sessions.push_back(s2);

    executor.register_session(s1);
    executor.register_session(s2);

    std::vector<cutwidth::GlobalDFSExecutor::EpochAdmitRequest> requests;

    cutwidth::GlobalDFSExecutor::EpochAdmitRequest r1;
    r1.session = s1;
    r1.threshold = 10;
    r1.permits = 2;
    r1.work_units = 100;

    cutwidth::GlobalDFSExecutor::EpochAdmitRequest r2;
    r2.session = s2;
    r2.threshold = 11;
    r2.permits = 2;
    r2.work_units = 100;

    requests.push_back(r1);
    requests.push_back(r2);

    auto ids = executor.grant_epochs(requests);
    executor.drain_epochs(ids);

    require(!(*failed), "A lease executed before all batched epochs were opened!");

    require(s1->get_contract().concurrency_target == 2, "s1 contract concurrency target mismatch");
    require(s2->get_contract().concurrency_target == 2, "s2 contract concurrency target mismatch");
    require(s1->get_contract().total_work_units == 100, "s1 contract work units mismatch");
    require(s2->get_contract().total_work_units == 100, "s2 contract work units mismatch");

    executor.unregister_session(s1);
    executor.unregister_session(s2);
}

void test_external_concurrency_target_consistency() {
    const cutwidth::Graph graph(4, {{0,1},{1,2},{2,3}});
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    cutwidth::DecisionOptions options;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.failed_state_cache_memory_bytes = 1U << 20;

    cutwidth::ParallelDecisionSession session(graph, optimum, options, 4, true);

    cutwidth::EpochContract contract;
    contract.epoch_id = 42;
    contract.threshold = optimum;
    contract.generation = 1;
    contract.concurrency_target = 4;
    contract.total_work_units = 2;
    contract.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    session.begin_external_epoch(contract);

    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < 4; ++i) {
        threads.emplace_back([&session, i] {
            (void)session.run_external_lease(i);
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    auto event = session.finish_external_epoch();
    require(event.status != cutwidth::SessionStatus::unresolved || event.reason == cutwidth::SessionYieldReason::quantum_complete,
            "external concurrency consistency test failed");
}

struct LeaseStartGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
};

class SignallingSession final : public cutwidth::GlobalDFSSession {
public:
    SignallingSession(std::shared_ptr<cutwidth::ParallelGlobalDFSSession> inner,
                      std::shared_ptr<LeaseStartGate> gate)
        : inner_(std::move(inner)), gate_(std::move(gate)) {}
    void prepare(std::uint64_t generation, std::uint32_t threshold) override {
        inner_->prepare(generation, threshold);
    }
    void open_epoch(const cutwidth::EpochContract& contract) override {
        inner_->open_epoch(contract);
    }
    cutwidth::LeaseOutcome run_one_lease(
        std::size_t worker_id, std::chrono::steady_clock::time_point deadline) override {
        {
            std::lock_guard lock(gate_->mutex);
            gate_->started = true;
        }
        gate_->cv.notify_all();
        return inner_->run_one_lease(worker_id, deadline);
    }
    void quiesce() override { inner_->quiesce(); }
    bool has_work() const override { return inner_->has_work(); }
    bool has_runnable_work() const override { return inner_->has_runnable_work(); }
    std::uint32_t threshold() const override { return inner_->threshold(); }
    std::uint64_t generation() const override { return inner_->generation(); }
    void revoke() override { inner_->revoke(); }
    bool is_revoked() const override { return inner_->is_revoked(); }
    double busy_worker_seconds() const override { return inner_->busy_worker_seconds(); }
    double allocated_worker_seconds() const override { return inner_->allocated_worker_seconds(); }
    void increment_steal_reservation() override { inner_->increment_steal_reservation(); }
    void decrement_steal_reservation() override { inner_->decrement_steal_reservation(); }

private:
    std::shared_ptr<cutwidth::ParallelGlobalDFSSession> inner_;
    std::shared_ptr<LeaseStartGate> gate_;
};

class AwaitRecipientSession final : public cutwidth::GlobalDFSSession {
public:
    explicit AwaitRecipientSession(std::shared_ptr<LeaseStartGate> gate)
        : gate_(std::move(gate)) {}
    void prepare(std::uint64_t generation, std::uint32_t threshold) override {
        generation_ = generation;
        threshold_ = threshold;
    }
    cutwidth::LeaseOutcome run_one_lease(
        std::size_t, std::chrono::steady_clock::time_point deadline) override {
        std::unique_lock lock(gate_->mutex);
        gate_->cv.wait_until(lock, deadline, [&] { return gate_->started; });
        cutwidth::LeaseOutcome outcome;
        outcome.status = cutwidth::LeaseOutcome::terminal;
        return outcome;
    }
    void quiesce() override {}
    bool has_work() const override { return true; }
    bool has_runnable_work() const override { return true; }
    std::uint32_t threshold() const override { return threshold_; }
    std::uint64_t generation() const override { return generation_; }
    void revoke() override { revoked_ = true; }
    bool is_revoked() const override { return revoked_; }
    double busy_worker_seconds() const override { return 0.0; }
    double allocated_worker_seconds() const override { return 0.0; }

private:
    std::shared_ptr<LeaseStartGate> gate_;
    std::uint32_t threshold_ = 0;
    std::uint64_t generation_ = 0;
    bool revoked_ = false;
};

void test_cross_session_stealing_and_telemetry() {
    std::vector<std::pair<cutwidth::Graph::Vertex, cutwidth::Graph::Vertex>> edges;
    for (cutwidth::Graph::Vertex u = 0; u < 8; ++u)
        for (cutwidth::Graph::Vertex v = u + 1; v < 8; ++v)
            edges.emplace_back(u, v);
    const cutwidth::Graph graph(8, std::move(edges));
    const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
    require(optimum == 16, "K8 oracle no longer supplies the steal test bounds");

    cutwidth::DecisionOptions options;
    options.use_failed_state_cache = true;
    options.cache_mode = cutwidth::CacheMode::fixed_threshold;
    options.failed_state_cache_memory_bytes = 1U << 20;

    cutwidth::GlobalDFSExecutor executor(4, std::chrono::milliseconds(20));
    auto gate = std::make_shared<LeaseStartGate>();
    auto proof_forest = std::make_shared<cutwidth::ParallelDecisionSession>(
        graph, optimum - 1U, options, 4, true);
    auto shallow = std::make_shared<AwaitRecipientSession>(gate);
    auto proof_inner = std::make_shared<cutwidth::ParallelGlobalDFSSession>(
        proof_forest, 4096);
    auto proof = std::make_shared<SignallingSession>(proof_inner, gate);
    executor.register_session(shallow);
    executor.register_session(proof);
    const auto proof_generation = executor.bind_session(proof, optimum - 1U);

    cutwidth::GlobalDFSExecutor::EpochAdmitRequest shallow_request;
    shallow_request.session = shallow;
    shallow_request.threshold = optimum;
    shallow_request.permits = 3;
    shallow_request.work_units = 64;
    shallow_request.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);

    cutwidth::GlobalDFSExecutor::EpochAdmitRequest proof_request;
    proof_request.session = proof;
    proof_request.threshold = optimum - 1U;
    proof_request.permits = 1;
    proof_request.work_units = 4096;
    proof_request.deadline = shallow_request.deadline;

    const auto epochs = executor.grant_epochs({shallow_request, proof_request});
    executor.drain_epochs(epochs);

    const auto telemetry = executor.get_telemetry();
    require(executor.generation(optimum - 1U) == proof_generation,
            "cross-session steal changed proof generation");
    require(telemetry.cross_session_steals != 0,
            "idle shallow-threshold leases did not steal proof work");
    require(telemetry.useful_leases != 0 &&
                telemetry.peak_active_leases <= executor.worker_count(),
            "cross-session telemetry contradicted physical pool capacity");
    require(telemetry.per_threshold_useful_work.at(optimum - 1U) <=
                proof_request.work_units,
            "stolen proof work multiplied scheduler credit");
    require(proof_forest->status() == cutwidth::SessionStatus::infeasible,
            "stolen exact proof disagreed with the independent K8 oracle");

    executor.unregister_session(shallow);
    executor.unregister_session(proof);
}

} // namespace

int main() {
    try {
        test_no_work_pre_grant();
        std::cout << "test_no_work_pre_grant passed.\n";

        test_capacity_bounded();
        std::cout << "test_capacity_bounded passed.\n";

        test_under_serviced_granted_session();
        std::cout << "test_under_serviced_granted_session passed.\n";

        test_no_work_post_drain();
        std::cout << "test_no_work_post_drain passed.\n";

        test_permit_identity_preserved();
        std::cout << "test_permit_identity_preserved passed.\n";

        test_real_parallel_forest_adapter();
        std::cout << "test_real_parallel_forest_adapter passed.\n";

        test_auxiliary_work_interleaves_with_dfs_under_shared_capacity();
        std::cout << "test_auxiliary_work_interleaves_with_dfs_under_shared_capacity passed.\n";

        test_batched_admission_order();
        std::cout << "test_batched_admission_order passed.\n";

        test_external_concurrency_target_consistency();
        std::cout << "test_external_concurrency_target_consistency passed.\n";

        test_cross_session_stealing_and_telemetry();
        std::cout << "test_cross_session_stealing_and_telemetry passed.\n";

        std::cout << "All permit-based executor tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
