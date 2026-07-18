#include "parallel_decision_session.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef CUTWIDTH_HAVE_ONETBB
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

namespace cutwidth {

std::atomic<std::int64_t> active_node_count{0};

namespace {

struct ActiveWorkerGuard {
    std::atomic<std::size_t>& count;
    std::condition_variable& wake;
    ~ActiveWorkerGuard() {
        count.fetch_sub(1);
        wake.notify_all();
    }
};

void add_delta(DecisionStats& total, const DecisionStats& part) {
    total.nodes_expanded += part.nodes_expanded;
    total.children_rejected_by_cut += part.children_rejected_by_cut;
    total.failed_cache_hits += part.failed_cache_hits;
    total.failed_cache_queries += part.failed_cache_queries;
    total.failed_states_recorded += part.failed_states_recorded;
    total.unique_canonical_claims += part.unique_canonical_claims;
    total.duplicate_ownership_waits += part.duplicate_ownership_waits;
    total.ownership_saturation += part.ownership_saturation;
    total.twin_symmetric_children_skipped += part.twin_symmetric_children_skipped;
    total.depth_two_lookahead_checks += part.depth_two_lookahead_checks;
    total.children_rejected_by_depth_two_lookahead +=
        part.children_rejected_by_depth_two_lookahead;
    total.node_state_updates += part.node_state_updates;
    total.residual_histogram_updates += part.residual_histogram_updates;
    total.partial_bounds.evaluations += part.partial_bounds.evaluations;
    total.partial_bounds.residual_degree_evaluations +=
        part.partial_bounds.residual_degree_evaluations;
    total.partial_bounds.edge_distance_area_evaluations +=
        part.partial_bounds.edge_distance_area_evaluations;
    total.partial_bounds.degree_distance_area_evaluations +=
        part.partial_bounds.degree_distance_area_evaluations;
    total.partial_bounds.degeneracy_evaluations += part.partial_bounds.degeneracy_evaluations;
    total.partial_bounds.residual_degree_prunes += part.partial_bounds.residual_degree_prunes;
    total.partial_bounds.edge_distance_area_prunes +=
        part.partial_bounds.edge_distance_area_prunes;
    total.partial_bounds.degree_distance_area_prunes +=
        part.partial_bounds.degree_distance_area_prunes;
    total.partial_bounds.degeneracy_prunes += part.partial_bounds.degeneracy_prunes;
    total.partial_bounds.expensive_slack_gate_skips +=
        part.partial_bounds.expensive_slack_gate_skips;
    total.configured_proof_regions_bound = std::max(total.configured_proof_regions_bound, part.configured_proof_regions_bound);
    total.resolved_proof_regions_bound = std::max(total.resolved_proof_regions_bound, part.resolved_proof_regions_bound);
    total.peak_proof_regions = std::max(total.peak_proof_regions, part.peak_proof_regions);
    total.suppressed_donations += part.suppressed_donations;
    total.residual_dp_attempts += part.residual_dp_attempts;
    total.residual_dp_admissions += part.residual_dp_admissions;
    total.residual_dp_governor_or_cap_rejections += part.residual_dp_governor_or_cap_rejections;
    total.residual_dp_completed_tails += part.residual_dp_completed_tails;
    total.residual_dp_infeasible_prunes += part.residual_dp_infeasible_prunes;
    total.residual_dp_feasible_witnesses += part.residual_dp_feasible_witnesses;
    total.residual_dp_states += part.residual_dp_states;
    total.residual_dp_peak_bytes = std::max(total.residual_dp_peak_bytes, part.residual_dp_peak_bytes);
    total.residual_dp_seconds += part.residual_dp_seconds;
    total.residual_dp_cold_restarts += part.residual_dp_cold_restarts;
    total.dfs_min_remaining_vertices = std::min(total.dfs_min_remaining_vertices, part.dfs_min_remaining_vertices);
    total.best_next_bucket_checks += part.best_next_bucket_checks;
    total.best_next_bucket_parent_prunes += part.best_next_bucket_parent_prunes;
    total.best_next_bucket_candidates_avoided += part.best_next_bucket_candidates_avoided;
    total.candidate_scan_checks += part.candidate_scan_checks;
    total.candidate_index_gathers += part.candidate_index_gathers;
    total.candidate_index_bucket_slots_visited +=
        part.candidate_index_bucket_slots_visited;
    total.candidate_index_vertices_emitted += part.candidate_index_vertices_emitted;
    total.candidate_index_forward_updates += part.candidate_index_forward_updates;
    total.candidate_index_rollback_updates += part.candidate_index_rollback_updates;
    total.candidate_index_cross_checks += part.candidate_index_cross_checks;
    total.local_continuation_calls += part.local_continuation_calls;
    total.local_continuation_slack_gate_skips +=
        part.local_continuation_slack_gate_skips;
    total.local_continuation_branch_gate_skips +=
        part.local_continuation_branch_gate_skips;
    total.local_continuation_inconclusive += part.local_continuation_inconclusive;
    total.local_continuation_states += part.local_continuation_states;
    total.local_continuation_parent_prunes += part.local_continuation_parent_prunes;
    total.local_continuation_nanoseconds += part.local_continuation_nanoseconds;
    total.local_continuation_cross_checks += part.local_continuation_cross_checks;
    merge_dfs_diagnostics(total.dfs_diagnostics, part.dfs_diagnostics);
}

} // namespace

class ParallelDecisionSession::Impl {
public:
    struct Node {
        static DecisionOptions owned(DecisionOptions options, std::uint64_t id) {
            options.ownership_id = id;
            return options;
        }
        Node(const Graph& graph, std::uint32_t threshold,
             const DecisionOptions& options, std::uint64_t id)
            : id(id), session(std::make_unique<DecisionSession>(
                  graph, threshold, owned(options, id))) {
            active_node_count.fetch_add(1, std::memory_order_relaxed);
        }
        Node(const Graph& graph, const SessionSnapshot& snapshot,
             const DecisionOptions& options, std::uint64_t id, bool defer = false)
            : id(id) {
            if (defer) deferred_snapshot = snapshot;
            else session = std::make_unique<DecisionSession>(
                graph, snapshot, owned(options, id));
            active_node_count.fetch_add(1, std::memory_order_relaxed);
        }
        ~Node() {
            active_node_count.fetch_sub(1, std::memory_order_relaxed);
        }
        DecisionSession& materialize(const Graph& graph,
                                     const DecisionOptions& options) {
            if (!session) {
                session = std::make_unique<DecisionSession>(
                    graph, *deferred_snapshot, owned(options, id));
                deferred_snapshot.reset();
            }
            return *session;
        }
        std::uint64_t id;
        std::mutex mutex;
        std::unique_ptr<DecisionSession> session;
        std::optional<SessionSnapshot> deferred_snapshot;
        std::weak_ptr<Node> parent;
        std::atomic<bool> queued{false};
        std::atomic<bool> active{false};
        std::atomic<bool> rerun_requested{false};
        // Guarded by mutex. A reservation is only ever taken while inactive,
        // but pop_ready checks it again after claiming active to close the
        // admission race with a concurrent control operation.
        bool unstarted_reserved = false;
        std::uint64_t reservation_id = 0;
        bool started = false;
        bool terminal_notified = false;
    };

    struct BootstrapState {
        std::vector<Graph::Vertex> path;
        std::vector<std::uint64_t> words;
        std::uint32_t cut = 0;
    };
    struct WordsHash {
        std::size_t operator()(const std::vector<std::uint64_t>& words) const noexcept {
            return static_cast<std::size_t>(hash_words(words));
        }
    };

    std::vector<BootstrapState> build_canonical_frontier() {
        if (graph.size() == 0) {
            current_status = SessionStatus::feasible;
            return {};
        }
        const auto target = worker_count >
                std::numeric_limits<std::size_t>::max() / worker_count
            ? std::numeric_limits<std::size_t>::max()
            : worker_count * worker_count;
        std::vector<BootstrapState> current;
        current.push_back({{}, std::vector<std::uint64_t>(graph.word_count(), 0), 0});
        while (current.size() < target) {
            std::vector<BootstrapState> next;
            std::unordered_set<std::vector<std::uint64_t>, WordsHash> unique;
            bool exceeded = false;
            for (const auto& state : current) {
                for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex) {
                    const auto wi = static_cast<std::size_t>(vertex / 64U);
                    const auto mask = std::uint64_t{1} << (vertex % 64U);
                    if ((state.words[wi] & mask) != 0) continue;
                    std::uint32_t before = 0;
                    const auto adjacency = graph.adjacency_words(vertex);
                    for (std::size_t word = 0; word < state.words.size(); ++word)
                        before += static_cast<std::uint32_t>(
                            std::popcount(adjacency[word] & state.words[word]));
                    const auto candidate_cut = static_cast<std::int64_t>(state.cut) +
                        graph.degree(vertex) - 2 * static_cast<std::int64_t>(before);
                    if (candidate_cut > threshold) continue;
                    auto words = state.words;
                    words[wi] |= mask;
                    if (!unique.insert(words).second) continue;
                    auto path = state.path;
                    path.push_back(vertex);
                    if (path.size() == graph.size()) {
                        current_status = SessionStatus::feasible;
                        solution = std::move(path);
                        return {};
                    }
                    if (next.size() >= resolved_proof_regions_bound) {
                        exceeded = true;
                        break;
                    }
                    next.push_back({std::move(path), std::move(words),
                        static_cast<std::uint32_t>(candidate_cut)});
                }
                if (exceeded) break;
            }
            if (exceeded) {
                break;
            }
            if (next.empty()) {
                current_status = SessionStatus::infeasible;
                return {};
            }
            current = std::move(next);
        }
        std::sort(current.begin(), current.end(), [&](const auto& a, const auto& b) {
            if (a.cut != b.cut)
                return options.use_fail_first_candidate_order ? a.cut > b.cut
                                                               : a.cut < b.cut;
            return a.path < b.path;
        });
        return current;
    }

    void initialize_root_regions() {
        auto frontier = build_canonical_frontier();
        if (current_status != SessionStatus::unresolved) return;
        if (frontier.size() == 1 && frontier.front().path.empty()) {
            auto root_node = std::make_shared<Node>(
                graph, threshold, options, next_node_id.fetch_add(1));
            root = root_node;
            nodes.push_back(root_node);
            nodes_by_id.emplace(root_node->id, root_node);
            enqueue(root_node);
            return;
        }
        SessionSnapshot parent_snapshot;
        parent_snapshot.threshold = threshold;
        parent_snapshot.status = SessionStatus::unresolved;
        parent_snapshot.unfinished_regions = frontier.size();
        parent_snapshot.external_regions = frontier.size();
        parent_snapshot.continuation_partitioned = true;
        auto root_node = std::make_shared<Node>(
            graph, parent_snapshot, options, next_node_id.fetch_add(1));
        root = root_node;
        nodes.push_back(root_node);
        nodes_by_id.emplace(root_node->id, root_node);
        for (auto& state : frontier) {
            SessionSnapshot child_snapshot;
            child_snapshot.threshold = threshold;
            child_snapshot.status = SessionStatus::unresolved;
            child_snapshot.path = std::move(state.path);
            child_snapshot.unfinished_regions = 1;
            SessionFrameSnapshot frame;
            frame.cut = state.cut;
            child_snapshot.frames.push_back(std::move(frame));
            auto child = std::make_shared<Node>(graph, child_snapshot, options,
                next_node_id.fetch_add(1), true);
            child->parent = root_node;
            nodes.push_back(child);
            nodes_by_id.emplace(child->id, child);
            enqueue(child);
        }
        pending_bootstrap_donations = frontier.size();
    }

    Impl(const Graph& graph, std::uint32_t threshold, DecisionOptions options,
         std::size_t worker_count, bool external_workers)
        : graph(graph), threshold(threshold), options(std::move(options)),
          worker_count(worker_count), external_workers(external_workers) {
        resolved_proof_regions_bound = this->options.max_proof_regions == 0
            ? std::max<std::size_t>(worker_count, 8 * worker_count)
            : this->options.max_proof_regions;
        if (worker_count == 0) throw std::invalid_argument("parallel session needs workers");
        validate_recursive_kernel();
        initialize_acceleration();
        initialize_recursive_workers();
        if (this->options.canonical_frontier_bootstrap) initialize_root_regions();
        else {
            auto root_node = std::make_shared<Node>(
                graph, threshold, this->options, next_node_id.fetch_add(1));
            root = root_node;
            nodes.push_back(root_node);
            nodes_by_id.emplace(root_node->id, root_node);
            enqueue(root_node);
        }
        live_nodes_count.store(nodes.size(), std::memory_order_relaxed);
        peak_live_nodes_count.store(nodes.size(), std::memory_order_relaxed);
        if (!external_workers) start_workers();
    }

    Impl(const Graph& graph, const ParallelDecisionSnapshot& snapshot,
         DecisionOptions options, std::size_t worker_count, bool external_workers)
        : graph(graph), threshold(snapshot.threshold), options(std::move(options)),
          worker_count(worker_count), external_workers(external_workers),
          current_status(snapshot.status), solution(snapshot.ordering) {
        resolved_proof_regions_bound = this->options.max_proof_regions == 0
            ? std::max<std::size_t>(worker_count, 8 * worker_count)
            : this->options.max_proof_regions;
        if (worker_count == 0) throw std::invalid_argument("parallel session needs workers");
        if (snapshot.status == SessionStatus::cancelled || snapshot.regions.empty())
            throw std::invalid_argument("invalid parallel decision snapshot");
        if (snapshot.status == SessionStatus::feasible &&
            (!graph.validate_ordering(solution) ||
             graph.ordering_cutwidth(solution) > threshold))
            throw std::invalid_argument("parallel snapshot witness is invalid");
        validate_recursive_kernel();
        initialize_acceleration(snapshot.fixed_cache);
        initialize_recursive_workers();
        std::unordered_map<std::uint64_t, std::shared_ptr<Node>> restored;
        std::uint64_t maximum_id = 0;
        for (const auto& region : snapshot.regions) {
            if (region.region_id == 0 || region.session.threshold != threshold ||
                !restored.emplace(region.region_id, nullptr).second)
                throw std::invalid_argument("parallel snapshot has invalid region identity");
            const bool defer = this->options.recursive_coarse_kernel &&
                region.session.status == SessionStatus::unresolved &&
                region.session.unfinished_regions == 1 &&
                region.session.external_regions == 0 &&
                region.session.pending.empty() && region.session.frames.size() == 1;
            auto node = std::make_shared<Node>(
                graph, region.session, this->options, region.region_id, defer);
            restored[region.region_id] = node;
            nodes.push_back(node);
            nodes_by_id.emplace(node->id, node);
            maximum_id = std::max(maximum_id, node->id);
        }
        std::size_t roots = 0;
        std::unordered_map<std::uint64_t, std::uint64_t> child_counts;
        std::unordered_map<std::uint64_t, std::uint64_t> parent_ids;
        for (const auto& region : snapshot.regions) {
            auto node = restored.at(region.region_id);
            parent_ids.emplace(region.region_id, region.parent_region_id);
            if (region.parent_region_id == 0) {
                root = node;
                ++roots;
            } else {
                const auto parent = restored.find(region.parent_region_id);
                if (parent == restored.end() || parent->second == node)
                    throw std::invalid_argument("parallel snapshot has missing parent");
                node->parent = parent->second;
                ++child_counts[region.parent_region_id];
            }
        }
        if (roots != 1) throw std::invalid_argument("parallel snapshot must have one root");
        for (const auto& region : snapshot.regions)
            if (region.session.external_regions != child_counts[region.region_id])
                throw std::invalid_argument(
                    "parallel snapshot child count contradicts region accounting");
        // Every parent chain must terminate at the unique root.
        for (const auto& [id, unused] : parent_ids) {
            (void)unused;
            std::size_t hops = 0;
            auto cursor = id;
            while (cursor != 0) {
                if (++hops > nodes.size())
                    throw std::invalid_argument("parallel snapshot region cycle");
                cursor = parent_ids.at(cursor);
            }
        }
        next_node_id.store(maximum_id + 1U);
        for (const auto& node : nodes)
            if (node->deferred_snapshot ||
                (node->session->status() == SessionStatus::unresolved &&
                 !node->session->waiting_for_external_regions()))
                enqueue(node);
        live_nodes_count.store(nodes.size(), std::memory_order_relaxed);
        peak_live_nodes_count.store(nodes.size(), std::memory_order_relaxed);
        if (!external_workers) start_workers();
    }

    void initialize_acceleration(
        const std::optional<ShardedFixedThresholdDynamicCacheSnapshot>& restored = {}) {
        if (!this->options.shared_twins)
            this->options.shared_twins = make_decision_twin_table(
                graph, this->options.use_twin_symmetry);
        if (this->options.use_failed_state_cache) {
            bool admitted = true;
            if (this->options.memory_governor) {
                if (this->options.failed_state_cache_memory_bytes == 0) admitted = false;
                else {
                    cache_lease = this->options.memory_governor->try_acquire(
                        "parallel-threshold-cache-" + std::to_string(threshold),
                        this->options.failed_state_cache_memory_bytes);
                    admitted = cache_lease.has_value();
                }
            }
            if (admitted) {
                const auto shards = std::max(
                    this->options.parallel_min_cache_shards,
                    worker_count * this->options.parallel_cache_shards_per_thread);
                // Ownership is an advisory optimization inside the same cache
                // entitlement. Reserve a dimensionless fraction and derive its
                // capacity solely from projected bytes per full key.
                const auto ownership_bytes = this->options.use_canonical_ownership
                    ? this->options.failed_state_cache_memory_bytes / 16U : 0U;
                const auto cache_bytes = this->options.failed_state_cache_memory_bytes -
                    ownership_bytes;
                DecisionCacheOptions cache_options{
                    this->options.failed_state_cache_limit, cache_bytes};
                cache_options.replacement = this->options.cache_replacement;
                cache_options.replacement_page_capacity =
                    this->options.cache_replacement_page_capacity;
                if (restored) {
                    if (this->options.cache_mode != CacheMode::fixed_threshold ||
                        restored->threshold != threshold ||
                        restored->word_count != std::max<std::size_t>(1, graph.word_count()))
                        throw std::invalid_argument("parallel cache snapshot identity mismatch");
                    shared_cache = std::make_shared<ShardedFixedThresholdDynamicCache>(
                        ShardedFixedThresholdDynamicCache::restore(*restored));
                } else if (this->options.cache_mode == CacheMode::cross_threshold) {
                    shared_dynamic_cache = std::make_shared<ShardedDynamicDecisionCache>(
                        std::max<std::size_t>(1, graph.word_count()), shards,
                        cache_options);
                } else {
                    shared_cache = std::make_shared<ShardedFixedThresholdDynamicCache>(
                        std::max<std::size_t>(1, graph.word_count()), shards, threshold,
                        cache_options);
                }
                const auto projected_entry_bytes =
                    std::max<std::size_t>(1, graph.word_count()) * sizeof(std::uint64_t) +
                    sizeof(std::uint64_t) * 4U + sizeof(std::vector<std::uint64_t>) * 2U;
                const auto ownership_capacity = ownership_bytes / projected_entry_bytes;
                if (this->options.use_canonical_ownership && ownership_capacity != 0)
                    ownership = std::make_shared<CanonicalOwnershipTable>(
                        std::max<std::size_t>(1, graph.word_count()), shards,
                        ownership_capacity);
                this->options.shared_fixed_cache = shared_cache;
                this->options.shared_dynamic_cache = shared_dynamic_cache;
                this->options.canonical_ownership = ownership;
            } else {
                this->options.use_failed_state_cache = false;
                if (this->options.recursive_coarse_kernel) {
                    this->options.recursive_coarse_kernel = false;
                    this->options.canonical_frontier_bootstrap = false;
                }
                if (this->options.memory_governor) {
                    this->options.memory_governor->report_memory_pressure();
                }
            }
        }
    }

    void validate_recursive_kernel() const {
        if (!options.recursive_coarse_kernel) return;
        if (!options.canonical_frontier_bootstrap)
            throw std::invalid_argument(
                "recursive coarse kernel requires a canonical frontier");
        if (!options.use_failed_state_cache)
            throw std::invalid_argument(
                "recursive coarse kernel requires a shared failed-state cache");
        if (options.cooperative_work_stealing || options.use_canonical_ownership)
            throw std::invalid_argument(
                "recursive coarse kernel does not use frame donation or ownership");
    }

    void initialize_recursive_workers() {
        if (!options.recursive_coarse_kernel) return;
        if (!shared_dynamic_cache && !shared_cache)
            throw std::logic_error("recursive coarse kernel has no shared cache");
        recursive_workers.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            if (shared_dynamic_cache)
                recursive_workers.push_back(
                    std::make_unique<RecursiveDynamicSubtreeWorker>(
                        graph, threshold, options, *shared_dynamic_cache));
            else
                recursive_workers.push_back(
                    std::make_unique<RecursiveDynamicSubtreeWorker>(
                        graph, threshold, options, *shared_cache));
        }
    }

    void start_workers() {
        if (options.parallel_runtime == ParallelRuntime::onetbb) {
#ifdef CUTWIDTH_HAVE_ONETBB
            one_tbb_arena = std::make_unique<oneapi::tbb::task_arena>(
                static_cast<int>(worker_count), 0);
            one_tbb_arena->initialize();
            return;
#else
            throw std::invalid_argument(
                "oneTBB parallel runtime requested but this build has no oneTBB support");
#endif
        }
        threads.reserve(worker_count);
        for (std::size_t id = 0; id < worker_count; ++id)
            threads.emplace_back([this, id] { worker_loop(id); });
    }

    ~Impl() {
        recursive_stop.store(true, std::memory_order_relaxed);
        {
            std::lock_guard lock(control_mutex);
            stopping = true;
            service_active = false;
        }
        work_cv.notify_all();
        for (auto& thread : threads) if (thread.joinable()) thread.join();
#ifdef CUTWIDTH_HAVE_ONETBB
        if (one_tbb_arena) one_tbb_arena->terminate();
#endif
    }

    void enqueue(const std::shared_ptr<Node>& node) {
        if (node->active.load(std::memory_order_acquire)) {
            node->rerun_requested.store(true, std::memory_order_release);
            if (!node->active.load(std::memory_order_acquire) &&
                node->rerun_requested.exchange(false, std::memory_order_acq_rel))
                enqueue(node);
            return;
        }
        bool expected = false;
        if (!node->queued.compare_exchange_strong(expected, true)) return;
        {
            std::lock_guard lock(queue_mutex);
            ready.push_back(node);
        }
        work_cv.notify_one();
    }

    std::shared_ptr<Node> pop_ready() {
        for (;;) {
            std::shared_ptr<Node> node;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                if (ready.empty()) return {};
                node = ready.front();
                ready.pop_front();
                // Lock order constraint: release queue_mutex before acquiring node->mutex
                // to prevent deadlocks against concurrent donations (node->mutex -> queue_mutex).
                lock.unlock();
            }
            node->queued.store(false);
            bool expected = false;
            if (node->active.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                // Prevent queue_mutex -> node->mutex lock order deadlock against the donation hook.
                std::lock_guard node_lock(node->mutex);
                if (!node->unstarted_reserved) return node;
                // A claimant won the control race. Leave its queued marker
                // clear and make the region invisible to this DFS epoch.
                node->active.store(false, std::memory_order_release);
                continue;
            }
            node->rerun_requested.store(true, std::memory_order_release);
        }
    }

    void wake_ownership_waiters() {
        if (!ownership) return;
        for (const auto id : ownership->take_ready_waiters()) {
            std::shared_ptr<Node> node;
            {
                std::lock_guard lock(nodes_mutex);
                const auto found = nodes_by_id.find(id);
                if (found != nodes_by_id.end()) node = found->second.lock();
            }
            if (node) enqueue(node);
        }
    }

    std::size_t ready_size() const {
        std::lock_guard lock(queue_mutex);
        return ready.size();
    }

    void publish_feasible(const std::vector<Graph::Vertex>& witness) {
        if (!graph.validate_ordering(witness) || graph.ordering_cutwidth(witness) > threshold)
            throw std::logic_error("parallel worker published invalid witness");
        recursive_stop.store(true, std::memory_order_relaxed);
        std::lock_guard lock(control_mutex);
        if (current_status == SessionStatus::unresolved) {
            solution = witness;
            current_status = SessionStatus::feasible;
        }
        work_cv.notify_all();
        done_cv.notify_all();
    }

    void notify_failure(std::shared_ptr<Node> node) {
        while (node) {
            if (current_status_atomic() != SessionStatus::unresolved) return;
            auto parent = node->parent.lock();
            if (!parent) {
                recursive_stop.store(true, std::memory_order_relaxed);
                std::lock_guard lock(control_mutex);
                if (current_status == SessionStatus::unresolved)
                    current_status = SessionStatus::infeasible;
                work_cv.notify_all();
                done_cv.notify_all();
                return;
            }
            bool parent_terminal = false;
            {
                std::lock_guard lock(parent->mutex);
                // A feasible sibling may have closed and released this parent
                // after the global-status check above. This child's later
                // failure is then irrelevant to the already terminal proof
                // forest.
                if (!parent->session) return;
                if (parent->session->status() == SessionStatus::feasible) return;
                if (parent->session->status() != SessionStatus::unresolved ||
                    parent->session->unfinished_regions() == 0)
                    throw std::logic_error(
                        "parallel failure reached closed parent region " +
                        std::to_string(parent->id) + " from child " +
                        std::to_string(node->id) + "; status=" +
                        std::to_string(static_cast<int>(parent->session->status())) +
                        "; unfinished=" +
                        std::to_string(parent->session->unfinished_regions()) +
                        "; parent_notified=" +
                        std::to_string(parent->terminal_notified));
                parent->session->resolve_external_failure();
                parent_terminal = parent->session->status() == SessionStatus::infeasible;
                if (parent_terminal) {
                    if (parent->terminal_notified)
                        throw std::logic_error("parallel region failure notified twice");
                    // Propagation itself consumes this terminal region. Mark it
                    // before it can also be observed from the ready queue.
                    parent->terminal_notified = true;
                }
                if (!parent_terminal && !parent->session->waiting_for_external_regions())
                    enqueue(parent);
            }
            if (!parent_terminal) return;
            {
                std::lock_guard lock(event_mutex);
                ++event_terminal_regions;
            }
            node = std::move(parent);
        }
    }

    void process_terminal(const std::shared_ptr<Node>& node) {
        SessionStatus node_status;
        std::vector<Graph::Vertex> witness;
        {
            std::lock_guard lock(node->mutex);
            if (node->terminal_notified) return;
            node_status = node->session->status();
            if (node_status == SessionStatus::unresolved) return;
            node->terminal_notified = true;
            if (node_status == SessionStatus::feasible)
                witness = node->session->ordering();
        }
        {
            std::lock_guard lock(event_mutex);
            ++event_terminal_regions;
        }
        if (node_status == SessionStatus::feasible) publish_feasible(witness);
        else if (node_status == SessionStatus::infeasible) notify_failure(node);
    }

    void compact_proof_forest() {
        std::lock_guard nodes_lock(nodes_mutex);
        std::size_t before_count = nodes.size();
        auto it = std::remove_if(nodes.begin(), nodes.end(), [](const std::shared_ptr<Node>& node) {
            if (!node) return true;
            std::lock_guard node_lock(node->mutex);
            bool unresolved = !node->terminal_notified &&
                              (node->deferred_snapshot.has_value() ||
                               (node->session && node->session->status() == SessionStatus::unresolved));
            bool active = node->active.load(std::memory_order_acquire) ||
                          node->queued.load(std::memory_order_acquire);
            bool reserved = node->unstarted_reserved;

            bool keep = unresolved || active || reserved;
            return !keep;
        });
        nodes.erase(it, nodes.end());
        std::size_t after_count = nodes.size();
        if (before_count > after_count) {
            live_nodes_count.fetch_sub(before_count - after_count, std::memory_order_relaxed);
        }

        for (auto map_it = nodes_by_id.begin(); map_it != nodes_by_id.end(); ) {
            if (map_it->second.expired()) {
                map_it = nodes_by_id.erase(map_it);
            } else {
                ++map_it;
            }
        }
    }

    // A service epoch is a global quiescent point: every worker has returned,
    // so each unresolved region must either be runnable or be waiting for an
    // externally owned child.  Reconstructing the ready set here closes the
    // small enqueue/active hand-off window without putting another lock or
    // atomic operation on the DFS hot path.  Terminal publication is also
    // replayed defensively; process_terminal is idempotent.
    void reconcile_quiescent_frontier() {
        if (current_status_atomic() != SessionStatus::unresolved) return;

        std::vector<std::shared_ptr<Node>> terminal;
        std::vector<std::shared_ptr<Node>> runnable;
        std::size_t waiting = 0, reserved = 0;
        {
            std::lock_guard nodes_lock(nodes_mutex);
            terminal.reserve(nodes.size());
            runnable.reserve(nodes.size());
            for (const auto& node : nodes) {
                std::lock_guard node_lock(node->mutex);
                if (node->terminal_notified) continue;
                if (node->unstarted_reserved) { ++reserved; continue; }
                if (node->deferred_snapshot) {
                    runnable.push_back(node);
                    continue;
                }
                if (!node->session) continue;
                if (node->session->status() != SessionStatus::unresolved) {
                    terminal.push_back(node);
                    continue;
                }
                if (node->session->waiting_for_external_regions()) ++waiting;
                else runnable.push_back(node);
            }
        }

        for (const auto& node : terminal) process_terminal(node);
        if (current_status_atomic() != SessionStatus::unresolved) return;

        compact_proof_forest();

        // Terminal propagation may have made an ancestor runnable.  Scan a
        // second time instead of trying to infer that transition from stale
        // counts collected above.
        runnable.clear();
        waiting = 0;
        reserved = 0;
        {
            std::lock_guard nodes_lock(nodes_mutex);
            for (const auto& node : nodes) {
                std::lock_guard node_lock(node->mutex);
                if (node->terminal_notified) continue;
                if (node->unstarted_reserved) { ++reserved; continue; }
                if (node->deferred_snapshot) {
                    runnable.push_back(node);
                    continue;
                }
                if (!node->session ||
                    node->session->status() != SessionStatus::unresolved)
                    continue;
                if (node->session->waiting_for_external_regions()) ++waiting;
                else runnable.push_back(node);
            }
        }
        for (const auto& node : runnable) enqueue(node);
        if (!runnable.empty()) return;
        // A bounded auxiliary arm owns the only live work. It must release or
        // retire its exact claim before DFS can make another proof step.
        if (reserved != 0) return;

        throw std::logic_error(
            "unresolved parallel proof forest has no runnable region; waiting=" +
            std::to_string(waiting) + ", reserved=" + std::to_string(reserved));
    }

    LeaseOutcome worker_epoch(std::size_t id) {
        LeaseOutcome outcome;
        if (current_status_atomic() != SessionStatus::unresolved) {
            outcome.status = LeaseOutcome::terminal;
            return outcome;
        }
        // An owned coarse epoch promises one initial claim per allocated
        // worker.  Let every worker reach the epoch boundary before the first
        // runnable continuation is consumed; otherwise a fast worker can
        // repeatedly requeue the sole root before a late OS thread observes
        // the service, defeating the coarse-frontier allocation contract.
        if (!external_workers) {
            std::unique_lock lock(control_mutex);
            ++epoch_arrivals;
            if (epoch_arrivals == worker_count) {
                epoch_start_released = true;
                work_cv.notify_all();
            } else {
                work_cv.wait(lock, [this] { return epoch_start_released || stopping; });
            }
            if (stopping) {
                outcome.status = LeaseOutcome::terminal;
                return outcome;
            }
        }
        bool used = false;
        double busy_seconds = 0.0;
        for (;;) {
                // An owned coarse epoch guarantees a distinct first claim to
                // every allocated worker when the canonical frontier supplies
                // it.  The arrival barrier alone is insufficient: a fast
                // thread can otherwise consume several one-unit claims before
                // a late peer leaves the barrier.  After its first real
                // claim, hold a worker out of the second round until every
                // peer has had that same opportunity.
                if (!external_workers && options.recursive_coarse_kernel && used) {
                    std::unique_lock lock(control_mutex);
                    work_cv.wait_until(lock, deadline, [this] {
                        return stopping || current_status != SessionStatus::unresolved ||
                            recursive_stop.load(std::memory_order_relaxed) ||
                            remaining_work.load(std::memory_order_relaxed) == 0 ||
                            first_claims_issued >= worker_count ||
                            std::chrono::steady_clock::now() >= deadline;
                    });
                    if (stopping || current_status != SessionStatus::unresolved ||
                        recursive_stop.load(std::memory_order_relaxed) ||
                        remaining_work.load(std::memory_order_relaxed) == 0 ||
                        std::chrono::steady_clock::now() >= deadline)
                        break;
                }
                if (current_status_atomic() != SessionStatus::unresolved) {
                    if (outcome.status != LeaseOutcome::useful) {
                        outcome.status = LeaseOutcome::terminal;
                    }
                    break;
                }
                if (options.recursive_coarse_kernel &&
                    recursive_stop.load(std::memory_order_relaxed)) break;
                if (std::chrono::steady_clock::now() >= deadline) break;
                auto remaining = remaining_work.load(std::memory_order_relaxed);
                if (remaining == 0) break;
                auto node = pop_ready();
                if (!node) {
                    if (active_workers.load() == 0) {
                        if (outcome.status != LeaseOutcome::useful) {
                            outcome.status = LeaseOutcome::no_runnable;
                        }
                        break;
                    }
                    // An executor-owned worker must not sleep on this forest
                    // while another worker owns its only runnable region. Its
                    // proof ownership stays here; the physical lease returns
                    // immediately so the global pool can service another live
                    // threshold and revisit after a sibling is donated.
                    if (external_workers) {
                        outcome.status = LeaseOutcome::no_runnable;
                        break;
                    }
                    std::unique_lock lock(control_mutex);
                    work_cv.wait_until(lock, deadline, [&] {
                        return stopping || current_status != SessionStatus::unresolved ||
                            remaining_work.load(std::memory_order_relaxed) == 0 ||
                            ready_size() != 0 || active_workers.load() == 0;
                    });
                    if (stopping) break;
                    continue;
                }
                {
                    std::lock_guard lock(node->mutex);
                    if (node->terminal_notified ||
                        (!node->session && !node->deferred_snapshot)) {
                        node->active.store(false, std::memory_order_release);
                        node->rerun_requested.store(false, std::memory_order_release);
                        continue;
                    }
                }
                // When one continuation is the entire runnable frontier, stop
                // after its first expansion so it can expose a sibling before
                // consuming a whole per-worker share. This is a work-stealing
                // bootstrap rule, not a graph-size or time cutoff.
                const auto target_workers = external_workers
                    ? std::max<std::size_t>(1, external_concurrency_target.load(std::memory_order_relaxed))
                    : worker_count;
                const auto live_frontier = ready_size() + active_workers.load() + 1U;
                const auto grain_divisor = external_workers
                    ? (target_workers > std::numeric_limits<std::size_t>::max() / 2
                        ? std::numeric_limits<std::size_t>::max()
                        : target_workers * 2)
                    : (worker_count > std::numeric_limits<std::size_t>::max() / worker_count
                        ? std::numeric_limits<std::size_t>::max()
                        : worker_count * worker_count);
                const auto frontier_target = target_workers;
                const bool underfilled_frontier = live_frontier < frontier_target;
                // Maintain a dimensionless N-way oversubscription of each
                // worker's nominal share. This bounds subtree imbalance and
                // creates frequent safe donation opportunities without a
                // graph-size, node-count, or time cutoff.
                // Owned workers use a one-node bootstrap while the frontier is
                // underfilled so that siblings are exposed before any one worker
                // consumes a coarse quantum; the safe-point hook then donates
                // the next unstarted sibling to fill the frontier.
                // External workers (TBB/executor pool) are not owned and already
                // rely on the safe-point donation condition to split siblings, so
                // they always receive the bounded coarse share to avoid degenerating
                // every claim to a single node whenever a donation is in progress.
                const auto coarse_share = std::max<std::uint64_t>(1,
                    remaining / std::max<std::size_t>(1, grain_divisor) +
                    (remaining % std::max<std::size_t>(1, grain_divisor) != 0));
                const auto share = (!external_workers && underfilled_frontier)
                    ? std::uint64_t{1} : coarse_share;
                auto claim = std::min(remaining, share);
                while (!remaining_work.compare_exchange_weak(
                    remaining, remaining - std::min(remaining, claim))) {
                    if (remaining == 0) { claim = 0; break; }
                    claim = std::min(remaining, share);
                }
                if (claim == 0) {
                    node->active.store(false, std::memory_order_release);
                    node->rerun_requested.store(false, std::memory_order_release);
                    enqueue(node);
                    break;
                }
                if (!external_workers && options.recursive_coarse_kernel && !used) {
                    std::lock_guard lock(control_mutex);
                    ++first_claims_issued;
                    work_cv.notify_all();
                }
                used = true;
                active_workers.fetch_add(1);
                ActiveWorkerGuard active_guard{active_workers, work_cv};
                SessionServiceEvent event;
                std::shared_ptr<Node> child;
                bool recursive_region = false;
                std::vector<Graph::Vertex> recursive_prefix;
                std::vector<Graph::Vertex> recursive_witness;
                std::uint32_t recursive_cut = 0;
                if (options.recursive_coarse_kernel) {
                    std::lock_guard lock(node->mutex);
                    // A lone coarse subtree cannot expose any claim to an
                    // idle worker.  Materialize only an underfilled frontier
                    // through the resumable DecisionSession path, whose safe
                    // point can detach one unexplored sibling.  Once the
                    // frontier reaches the allocated width, retain compact
                    // recursive ownership for the remaining coarse regions.
                    const auto live_frontier = ready_size() + active_workers.load();
                    const auto recursive_frontier_target = external_workers
                        ? std::max<std::size_t>(1, external_concurrency_target.load(
                              std::memory_order_relaxed))
                        : worker_count;
                    // A global executor permit is a bounded, replayable
                    // fragment.  The compact recursive worker only observes
                    // its deadline through geometric polls and has no work
                    // credit budget, so using it here could turn one lease
                    // into an arbitrarily long subtree.  External workers
                    // therefore stay on the resumable DecisionSession path,
                    // which consumes `claim` directly and can donate siblings.
                    if (live_frontier >= recursive_frontier_target &&
                        node->deferred_snapshot &&
                        node->deferred_snapshot->status == SessionStatus::unresolved &&
                        node->deferred_snapshot->unfinished_regions == 1 &&
                        node->deferred_snapshot->external_regions == 0 &&
                        node->deferred_snapshot->pending.empty() &&
                        node->deferred_snapshot->frames.size() == 1) {
                        recursive_region = true;
                        recursive_prefix = node->deferred_snapshot->path;
                        recursive_cut = node->deferred_snapshot->frames.front().cut;
                    }
                }
                const auto busy_started = std::chrono::steady_clock::now();
                if (recursive_region) {
                    const auto recursive = recursive_workers[id]->run(
                        recursive_prefix, recursive_cut, &recursive_stop, deadline,
                        external_workers
                            ? claim
                            : std::numeric_limits<std::uint64_t>::max());
                    event.threshold = threshold;
                    event.delta = recursive.stats;
                    if (recursive.status == DecisionStatus::feasible) {
                        event.status = SessionStatus::feasible;
                        event.reason = SessionYieldReason::terminal;
                        recursive_witness = recursive.ordering;
                    } else if (recursive.status == DecisionStatus::infeasible) {
                        event.status = SessionStatus::infeasible;
                        event.reason = SessionYieldReason::terminal;
                    } else {
                        event.status = SessionStatus::unresolved;
                        event.reason = std::chrono::steady_clock::now() >= deadline
                            ? SessionYieldReason::deadline
                            : SessionYieldReason::yield_requested;
                    }
                    event.right_censored = event.status == SessionStatus::unresolved;
                    // A bounded recursive claim keeps the fast path for the
                    // common case where a canonical subtree finishes inside
                    // its grain. If it exhausts that grain, materialize the
                    // same root as a resumable DecisionSession exactly once.
                    // Completed failures are already in the shared cache; the
                    // remaining hard tail now preserves its live stack across
                    // scheduler epochs instead of replaying from this root.
                    if (external_workers &&
                        recursive.status == DecisionStatus::timed_out &&
                        recursive.stats.nodes_expanded >= claim) {
                        std::lock_guard lock(node->mutex);
                        if (!node->session && node->deferred_snapshot)
                            (void)node->materialize(graph, options);
                    }
                    const std::uint64_t consumed_credit = [&]() {
                        std::uint64_t sum = event.delta.nodes_expanded;
                        if (std::numeric_limits<std::uint64_t>::max() - sum < event.delta.residual_dp_states) {
                            return std::numeric_limits<std::uint64_t>::max();
                        }
                        return sum + event.delta.residual_dp_states;
                    }();
                    if (consumed_credit < claim) {
                        remaining_work.fetch_add(
                            claim - consumed_credit,
                            std::memory_order_relaxed);
                    }
                } else {
                    std::lock_guard lock(node->mutex);
                    node->started = true;
                    auto& session = node->materialize(graph, options);
                    // A worker already inside a large service quantum must
                    // still react when another worker becomes idle. Requesting
                    // a cooperative yield at the next DFS safe point lets the
                    // owner detach a sibling without polling on a fixed time or
                    // node-count interval.
                    bool passed_entry_safe_point = false;
                    if (options.cooperative_work_stealing)
                        session.set_safe_point_hook([this,
                                session = &session,
                                &passed_entry_safe_point](std::uint64_t) {
                            if (!passed_entry_safe_point) {
                                passed_entry_safe_point = true;
                                return;
                            }
                            const auto target = external_workers
                                ? [&] {
                                      const auto base = std::max<std::size_t>(1,
                                          external_concurrency_target.load(
                                              std::memory_order_relaxed));
                                      const auto stolen = steal_reservation_count.load(
                                          std::memory_order_relaxed);
                                      return stolen > std::numeric_limits<std::size_t>::max() - base
                                          ? std::numeric_limits<std::size_t>::max()
                                          : base + stolen;
                                  }()
                                : worker_count;
                            if (ready_size() + active_workers.load() < target)
                                session->request_yield();
                        });
                    try {
                        event = session.service({claim, deadline});
                    } catch (...) {
                        session.set_safe_point_hook({});
                        throw;
                    }
                    session.set_safe_point_hook({});
                    const std::uint64_t consumed_credit = [&]() {
                        std::uint64_t sum = event.delta.nodes_expanded;
                        if (std::numeric_limits<std::uint64_t>::max() - sum < event.delta.residual_dp_states) {
                            return std::numeric_limits<std::uint64_t>::max();
                        }
                        return sum + event.delta.residual_dp_states;
                    }();
                    if (consumed_credit < claim) {
                        remaining_work.fetch_add(
                            claim - consumed_credit,
                            std::memory_order_relaxed);
                    }
                    if (event.status == SessionStatus::unresolved &&
                        event.reason != SessionYieldReason::ownership_wait) {
                        bool reserved = false;
                        std::size_t cur_val = live_nodes_count.load(std::memory_order_relaxed);
                        while (cur_val < resolved_proof_regions_bound) {
                            if (live_nodes_count.compare_exchange_weak(cur_val, cur_val + 1, std::memory_order_relaxed)) {
                                reserved = true;
                                std::size_t current_peak = peak_live_nodes_count.load(std::memory_order_relaxed);
                                while (cur_val + 1 > current_peak) {
                                    if (peak_live_nodes_count.compare_exchange_weak(current_peak, cur_val + 1, std::memory_order_relaxed)) {
                                        break;
                                    }
                                }
                                break;
                            }
                        }

                        if (!reserved) {
                            suppressed_donations.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            bool produced = false;
                            if (ready_size() + active_workers.load() < frontier_target &&
                                session.donate_unexplored_sibling()) {
                                if (auto snapshot = session.extract_pending_continuation()) {
                                    child = std::make_shared<Node>(
                                        graph, *snapshot, options, next_node_id.fetch_add(1));
                                    child->parent = node;
                                    {
                                        std::lock_guard event_lock(event_mutex);
                                        ++event_donations;
                                    }
                                    produced = true;
                                }
                            }
                            if (!produced) {
                                live_nodes_count.fetch_sub(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
                const std::uint64_t consumed_credit = [&]() {
                    std::uint64_t sum = event.delta.nodes_expanded;
                    if (std::numeric_limits<std::uint64_t>::max() - sum < event.delta.residual_dp_states) {
                        return std::numeric_limits<std::uint64_t>::max();
                    }
                    return sum + event.delta.residual_dp_states;
                }();
                outcome.status = LeaseOutcome::useful;
                outcome.consumed_work_units += consumed_credit;
                outcome.nodes_expanded += event.delta.nodes_expanded;

                busy_seconds += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - busy_started).count();
                {
                    std::lock_guard event_lock(event_mutex);
                    add_delta(event_delta, event.delta);
                }
                if (child) {
                    {
                        std::lock_guard nodes_lock(nodes_mutex);
                        nodes.push_back(child);
                        nodes_by_id.emplace(child->id, child);
                    }
                    enqueue(child);
                }
                bool runnable = false;
                if (event.status == SessionStatus::unresolved) {
                    std::lock_guard lock(node->mutex);
                    runnable = recursive_region ||
                        (event.reason != SessionYieldReason::ownership_wait &&
                         !node->session->waiting_for_external_regions());
                }
                if (event.status == SessionStatus::unresolved) {
                    node->active.store(false, std::memory_order_release);
                    const bool rerun = node->rerun_requested.exchange(
                        false, std::memory_order_acq_rel);
                    if (runnable || rerun) {
                        std::lock_guard lock(node->mutex);
                        if (recursive_region ||
                            (node->session->status() == SessionStatus::unresolved &&
                             !node->session->waiting_for_external_regions()))
                            enqueue(node);
                    }
                } else {
                    // Keep exclusive ownership until terminal publication is
                    // complete. A stale queued wakeup then observes the
                    // terminal marker and cannot service the region twice.
                    node->rerun_requested.store(false, std::memory_order_release);
                    if (recursive_region) {
                        {
                            std::lock_guard lock(node->mutex);
                            node->terminal_notified = true;
                            node->deferred_snapshot.reset();
                        }
                        {
                            std::lock_guard event_lock(event_mutex);
                            ++event_terminal_regions;
                        }
                        if (event.status == SessionStatus::feasible)
                            publish_feasible(recursive_witness);
                        else
                            notify_failure(node);
                    } else {
                        process_terminal(node);
                    }
                    node->active.store(false, std::memory_order_release);
                }
                wake_ownership_waiters();
                work_cv.notify_all();
                done_cv.notify_all();
        }
        if (used) {
            std::lock_guard lock(event_mutex);
            worker_used[id] = true;
            event_busy_worker_seconds += busy_seconds;
        }
        outcome.busy_seconds = busy_seconds;
        if (outcome.status != LeaseOutcome::useful) {
            if (stopping || current_status_atomic() != SessionStatus::unresolved) {
                outcome.status = LeaseOutcome::terminal;
            } else if (remaining_work.load(std::memory_order_relaxed) == 0) {
                outcome.status = LeaseOutcome::empty;
            } else if (ready_size() == 0 && active_workers.load() == 0) {
                outcome.status = LeaseOutcome::no_runnable;
            } else {
                outcome.status = LeaseOutcome::empty;
            }
        }
        return outcome;
    }

    void run_worker_epoch(std::size_t id) noexcept {
        try {
            worker_epoch(id);
        } catch (...) {
            {
                std::lock_guard lock(exception_mutex);
                if (!service_exception) service_exception = std::current_exception();
            }
            remaining_work.store(0);
            work_cv.notify_all();
        }
        {
            std::lock_guard lock(control_mutex);
            ++finished_workers;
        }
        done_cv.notify_all();
    }

    void worker_loop(std::size_t id) {
        std::uint64_t seen_epoch = 0;
        for (;;) {
            {
                std::unique_lock lock(control_mutex);
                work_cv.wait(lock, [&] {
                    return stopping || (service_active && epoch != seen_epoch);
                });
                if (stopping) return;
                seen_epoch = epoch;
            }
            run_worker_epoch(id);
        }
    }

    SessionStatus current_status_atomic() const {
        std::lock_guard lock(control_mutex);
        return current_status;
    }

    bool has_runnable_work() const {
        std::lock_guard lock(control_mutex);
        if (stopping) return false;
        if (current_status != SessionStatus::unresolved) return false;
        if (remaining_work.load(std::memory_order_relaxed) == 0) return false;
        // An executor steal needs a claim it can take now. Merely observing an
        // owner inside this forest caused idle workers to bounce between live
        // sessions and exhaust their handoff attempts before that owner
        // published a donated sibling.
        if (external_workers) return ready_size() > 0;
        return ready_size() > 0 || active_workers.load(std::memory_order_relaxed) > 0;
    }

    ParallelDecisionSnapshot snapshot(SnapshotPolicy policy = SnapshotPolicy::include_cache) const {
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            external_cv.wait(lease_lock, [this] { return external_active_leases == 0; });
        }
        ParallelDecisionSnapshot result;
        result.threshold = threshold;
        result.status = current_status_atomic();
        {
            std::lock_guard lock(control_mutex);
            result.ordering = solution;
        }
        if (policy == SnapshotPolicy::include_cache && shared_cache) result.fixed_cache = shared_cache->snapshot();
        std::lock_guard nodes_lock(nodes_mutex);
        for (const auto& node : nodes) {
            std::lock_guard node_lock(node->mutex);
            if (node->unstarted_reserved)
                throw std::logic_error(
                    "parallel snapshot requires all fragment claims released");
            ParallelRegionSnapshot region;
            region.region_id = node->id;
            if (const auto parent = node->parent.lock()) region.parent_region_id = parent->id;
            if (node->session) {
                if (node->session->status() != SessionStatus::unresolved) continue;
                region.session = node->session->quiesce_region_snapshot();
            } else if (node->deferred_snapshot) {
                region.session = *node->deferred_snapshot;
            } else continue;
            result.regions.push_back(std::move(region));
        }
        if (result.status == SessionStatus::unresolved && result.regions.empty())
            throw std::logic_error("unresolved parallel session has no live regions");
        return result;
    }

    std::optional<ParallelUnstartedFragment> inspect_unstarted_fragment() const {
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            if (external_active_leases != 0) return std::nullopt;
        }
        std::lock_guard nodes_lock(nodes_mutex);
        for (const auto& node : nodes) {
            std::lock_guard node_lock(node->mutex);
            if (node->unstarted_reserved || node->started ||
                node->active.load(std::memory_order_acquire) ||
                node->terminal_notified) continue;
            std::optional<SessionSnapshot> saved;
            if (node->deferred_snapshot) saved = *node->deferred_snapshot;
            else if (node->session && node->session->status() == SessionStatus::unresolved)
                saved = node->session->quiesce_region_snapshot();
            if (!saved || saved->unfinished_regions != 1 || saved->external_regions != 0 ||
                !saved->pending.empty()) continue;
            return ParallelUnstartedFragment{node->id, 0, std::move(*saved)};
        }
        return std::nullopt;
    }

    std::optional<ParallelUnstartedFragment> inspect_deepest_unstarted_fragment() const {
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            if (external_active_leases != 0) return std::nullopt;
        }
        std::lock_guard nodes_lock(nodes_mutex);
        std::shared_ptr<Node> best_node = nullptr;
        std::optional<SessionSnapshot> best_saved;
        std::size_t best_path_size = 0;
        bool found_any = false;

        for (const auto& node : nodes) {
            std::lock_guard node_lock(node->mutex);
            if (node->unstarted_reserved || node->started ||
                node->active.load(std::memory_order_acquire) ||
                node->terminal_notified) continue;
            std::optional<SessionSnapshot> saved;
            if (node->deferred_snapshot) saved = *node->deferred_snapshot;
            else if (node->session && node->session->status() == SessionStatus::unresolved)
                saved = node->session->quiesce_region_snapshot();
            if (!saved || saved->unfinished_regions != 1 || saved->external_regions != 0 ||
                !saved->pending.empty()) continue;

            std::size_t path_size = saved->path.size();
            if (!found_any || path_size > best_path_size || (path_size == best_path_size && node->id < best_node->id)) {
                best_node = node;
                best_saved = std::move(saved);
                best_path_size = path_size;
                found_any = true;
            }
        }
        if (found_any && best_node && best_saved) {
            return ParallelUnstartedFragment{best_node->id, 0, std::move(*best_saved)};
        }
        return std::nullopt;
    }

    static bool same_unstarted_identity(const SessionSnapshot& actual,
                                        const SessionSnapshot& expected) {
        return actual.threshold == expected.threshold &&
            actual.status == expected.status && actual.path == expected.path &&
            actual.ordering == expected.ordering && actual.frames == expected.frames &&
            actual.pending == expected.pending &&
            actual.unfinished_regions == expected.unfinished_regions &&
            actual.external_regions == expected.external_regions &&
            actual.continuation_partitioned == expected.continuation_partitioned &&
            actual.controller_quantum == expected.controller_quantum &&
            actual.controller_services == expected.controller_services &&
            actual.session_generation == expected.session_generation;
    }

    std::optional<SessionSnapshot> unstarted_snapshot_locked(const Node& node) const {
        std::optional<SessionSnapshot> saved;
        if (node.deferred_snapshot) saved = *node.deferred_snapshot;
        else if (node.session && node.session->status() == SessionStatus::unresolved)
            saved = node.session->quiesce_region_snapshot();
        if (!saved || saved->unfinished_regions != 1 || saved->external_regions != 0 ||
            !saved->pending.empty() || saved->frames.size() != 1)
            return std::nullopt;
        return saved;
    }

    std::optional<ParallelUnstartedFragment> claim_unstarted_fragment() {
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            if (external_epoch_active.load(std::memory_order_acquire) ||
                external_active_leases != 0) return std::nullopt;
        }
        std::lock_guard nodes_lock(nodes_mutex);
        for (const auto& node : nodes) {
            std::lock_guard node_lock(node->mutex);
            if (node->unstarted_reserved || node->started ||
                node->active.load(std::memory_order_acquire) || node->terminal_notified)
                continue;
            auto saved = unstarted_snapshot_locked(*node);
            if (!saved) continue;
            node->unstarted_reserved = true;
            const auto reservation = next_reservation_id.fetch_add(1,
                std::memory_order_relaxed);
            if (reservation == 0) throw std::overflow_error("parallel reservation id overflow");
            node->reservation_id = reservation;
            return ParallelUnstartedFragment{node->id, reservation, std::move(*saved)};
        }
        return std::nullopt;
    }

    std::optional<ParallelUnstartedFragment> claim_deepest_unstarted_fragment() {
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            if (external_epoch_active.load(std::memory_order_acquire) ||
                external_active_leases != 0) return std::nullopt;
        }
        std::lock_guard nodes_lock(nodes_mutex);
        std::shared_ptr<Node> best_node = nullptr;
        std::optional<SessionSnapshot> best_saved;
        std::size_t best_path_size = 0;
        bool found_any = false;

        for (const auto& node : nodes) {
            std::lock_guard node_lock(node->mutex);
            if (node->unstarted_reserved || node->started ||
                node->active.load(std::memory_order_acquire) || node->terminal_notified)
                continue;
            auto saved = unstarted_snapshot_locked(*node);
            if (!saved) continue;

            std::size_t path_size = saved->path.size();
            if (!found_any || path_size > best_path_size || (path_size == best_path_size && node->id < best_node->id)) {
                best_node = node;
                best_saved = std::move(saved);
                best_path_size = path_size;
                found_any = true;
            }
        }

        if (found_any && best_node && best_saved) {
            std::lock_guard node_lock(best_node->mutex);
            if (best_node->unstarted_reserved || best_node->started ||
                best_node->active.load(std::memory_order_acquire) || best_node->terminal_notified) {
                return std::nullopt;
            }
            best_node->unstarted_reserved = true;
            const auto reservation = next_reservation_id.fetch_add(1, std::memory_order_relaxed);
            if (reservation == 0) throw std::overflow_error("parallel reservation id overflow");
            best_node->reservation_id = reservation;
            return ParallelUnstartedFragment{best_node->id, reservation, std::move(*best_saved)};
        }
        return std::nullopt;
    }

    std::optional<ParallelUnstartedFragment> donate_and_claim_deepest_unstarted_fragment() {
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            if (external_epoch_active.load(std::memory_order_acquire) ||
                external_active_leases != 0) return std::nullopt;
        }

        std::size_t cur_val = live_nodes_count.load(std::memory_order_relaxed);
        bool capacity_reserved = false;
        while (cur_val < resolved_proof_regions_bound) {
            if (live_nodes_count.compare_exchange_weak(cur_val, cur_val + 1, std::memory_order_relaxed)) {
                capacity_reserved = true;
                std::size_t current_peak = peak_live_nodes_count.load(std::memory_order_relaxed);
                while (cur_val + 1 > current_peak) {
                    if (peak_live_nodes_count.compare_exchange_weak(current_peak, cur_val + 1, std::memory_order_relaxed)) {
                        break;
                    }
                }
                break;
            }
        }
        if (!capacity_reserved) {
            return std::nullopt;
        }

        std::lock_guard nodes_lock(nodes_mutex);

        struct CandidateDonor {
            std::shared_ptr<Node> node;
            std::size_t path_size;
        };
        std::vector<CandidateDonor> candidates;

        for (const auto& node : nodes) {
            std::lock_guard node_lock(node->mutex);
            if (!node->session) continue;
            if (node->active.load(std::memory_order_acquire) || node->unstarted_reserved ||
                !node->started || node->terminal_notified) continue;
            if (node->session->status() != SessionStatus::unresolved ||
                node->session->waiting_for_external_regions()) continue;

            auto snapshot = node->session->quiesce_region_snapshot();
            candidates.push_back({node, snapshot.path.size()});
        }

        std::sort(candidates.begin(), candidates.end(), [](const CandidateDonor& a, const CandidateDonor& b) {
            if (a.path_size != b.path_size) {
                return a.path_size > b.path_size;
            }
            return a.node->id < b.node->id;
        });

        for (const auto& candidate : candidates) {
            auto& node = candidate.node;
            std::lock_guard node_lock(node->mutex);

            if (!node->session || node->active.load(std::memory_order_acquire) || node->unstarted_reserved ||
                !node->started || node->terminal_notified ||
                node->session->status() != SessionStatus::unresolved ||
                node->session->waiting_for_external_regions()) continue;

            if (node->session->donate_unexplored_sibling()) {
                std::optional<SessionSnapshot> snapshot;
                try {
                    snapshot = node->session->extract_pending_continuation();
                } catch (...) {
                    try {
                        node->session->resolve_external_failure();
                    } catch (...) {}
                    live_nodes_count.fetch_sub(1, std::memory_order_relaxed);
                    throw;
                }
                if (snapshot) {
                    std::shared_ptr<Node> child;
                    try {
                        child = std::make_shared<Node>(
                            graph, *snapshot, options, next_node_id.fetch_add(1), true);
                        child->parent = node;
                    } catch (...) {
                        try {
                            node->session->resolve_external_failure();
                        } catch (...) {}
                        live_nodes_count.fetch_sub(1, std::memory_order_relaxed);
                        throw;
                    }

                    nodes.push_back(child);
                    nodes_by_id.emplace(child->id, child);

                    child->unstarted_reserved = true;
                    const auto reservation = next_reservation_id.fetch_add(1, std::memory_order_relaxed);
                    if (reservation == 0) throw std::overflow_error("parallel reservation id overflow");
                    child->reservation_id = reservation;

                    {
                        std::lock_guard event_lock(event_mutex);
                        ++event_donations;
                    }

                    return ParallelUnstartedFragment{child->id, reservation, std::move(*snapshot)};
                }
            }
        }

        live_nodes_count.fetch_sub(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    bool validate_claimed_fragment_locked(const Node& node,
                                          const ParallelUnstartedFragment& fragment) const {
        if (fragment.reservation_id == 0 || !node.unstarted_reserved ||
            node.reservation_id != fragment.reservation_id || node.started ||
            node.active.load(std::memory_order_acquire) || node.terminal_notified)
            return false;
        const auto actual = unstarted_snapshot_locked(node);
        return actual && same_unstarted_identity(*actual, fragment.session);
    }

    std::shared_ptr<Node> find_fragment_node(std::uint64_t region_id) const {
        std::lock_guard nodes_lock(nodes_mutex);
        const auto found = nodes_by_id.find(region_id);
        return found == nodes_by_id.end() ? std::shared_ptr<Node>{} : found->second.lock();
    }

    bool retire_unstarted_fragment(const ParallelUnstartedFragment& fragment) {
        if (fragment.reservation_id != 0) return false;
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            if (external_active_leases != 0) return false;
        }
        auto node = find_fragment_node(fragment.region_id);
        if (!node) return false;
        {
            std::lock_guard node_lock(node->mutex);
            if (node->unstarted_reserved || node->started ||
                node->active.load(std::memory_order_acquire) ||
                node->terminal_notified) return false;
            const auto actual = unstarted_snapshot_locked(*node);
            if (!actual || !same_unstarted_identity(*actual, fragment.session)) return false;
            if (!node->session) (void)node->materialize(graph, options);
            node->session->mark_certified_infeasible();
        }
        process_terminal(node);
        return true;
    }

    bool retire_claimed_unstarted_fragment(const ParallelUnstartedFragment& fragment) {
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            if (external_epoch_active.load(std::memory_order_acquire) ||
                external_active_leases != 0) return false;
        }
        auto node = find_fragment_node(fragment.region_id);
        if (!node) return false;
        {
            std::lock_guard node_lock(node->mutex);
            if (!validate_claimed_fragment_locked(*node, fragment)) return false;
            node->unstarted_reserved = false;
            node->reservation_id = 0;
            if (!node->session) (void)node->materialize(graph, options);
            node->session->mark_certified_infeasible();
        }
        process_terminal(node);
        return true;
    }

    bool release_claimed_unstarted_fragment(const ParallelUnstartedFragment& fragment) {
        std::unique_lock service_lock(service_mutex);
        if (external_workers) {
            std::unique_lock lease_lock(external_mutex);
            if (external_epoch_active.load(std::memory_order_acquire) ||
                external_active_leases != 0) return false;
        }
        auto node = find_fragment_node(fragment.region_id);
        if (!node) return false;
        {
            std::lock_guard node_lock(node->mutex);
            if (!validate_claimed_fragment_locked(*node, fragment)) return false;
            node->unstarted_reserved = false;
            node->reservation_id = 0;
        }
        enqueue(node);
        return true;
    }

    ParallelSessionEvent service(const SessionServiceBudget& budget) {
        if (external_workers)
            throw std::logic_error("external-worker session must use GlobalDFSExecutor");
        std::unique_lock call_lock(service_mutex);
        {
            std::lock_guard lock(event_mutex);
            event_delta = {};
            event_donations = pending_bootstrap_donations;
            pending_bootstrap_donations = 0;
            event_terminal_regions = 0;
            event_busy_worker_seconds = 0.0;
            worker_used.assign(worker_count, false);
        }
        {
            std::lock_guard lock(exception_mutex);
            service_exception = nullptr;
        }
        {
            std::lock_guard lock(exception_mutex);
            service_exception = nullptr;
        }
        DecisionCacheStats cache_before{};
        if (shared_cache) cache_before = shared_cache->stats();
        else if (shared_dynamic_cache) cache_before = shared_dynamic_cache->stats();
        {
            std::lock_guard lock(control_mutex);
            if (current_status != SessionStatus::unresolved) {
                ParallelSessionEvent terminal;
                terminal.status = current_status;
                terminal.reason = SessionYieldReason::terminal;
                terminal.donations = event_donations;
                terminal.right_censored = false;
                return terminal;
            }
            deadline = budget.absolute_deadline;
            recursive_stop.store(false, std::memory_order_relaxed);
            auto epoch_work = budget.work_units == 0 ? std::uint64_t{1}
                                                     : budget.work_units;
            // A coarse-recursive claim owns a complete canonical subtree, not
            // one DFS node.  Letting a small controller quantum cap the number
            // of claims therefore idles otherwise allocated workers even when
            // the ready proof forest is full.  Preserve geometric service, but
            // floor each coarse epoch at one claim per worker.
            if (options.recursive_coarse_kernel)
                epoch_work = std::max<std::uint64_t>(epoch_work, worker_count);
            remaining_work.store(epoch_work);
            finished_workers = 0;
            epoch_arrivals = 0;
            epoch_start_released = false;
            first_claims_issued = 0;
            service_active = true;
            ++epoch;
        }
        const auto service_started = std::chrono::steady_clock::now();
        if (options.parallel_runtime == ParallelRuntime::onetbb) {
#ifdef CUTWIDTH_HAVE_ONETBB
            // Use a structured task group inside the arena. Besides making the
            // service boundary an explicit join, this avoids handing detached
            // enqueue closures to the runtime while the caller immediately
            // begins observing epoch completion state.
            one_tbb_arena->execute([this] {
                oneapi::tbb::task_group group;
                for (std::size_t id = 0; id < worker_count; ++id)
                    group.run([this, id] { run_worker_epoch(id); });
                group.wait();
            });
#endif
        } else {
            work_cv.notify_all();
        }
        {
            std::unique_lock lock(control_mutex);
            done_cv.wait(lock, [&] { return finished_workers == worker_count; });
            service_active = false;
        }
        {
            std::lock_guard lock(exception_mutex);
            if (service_exception) std::rethrow_exception(service_exception);
        }
        reconcile_quiescent_frontier();
        ParallelSessionEvent result;
        result.status = current_status_atomic();
        result.reason = result.status != SessionStatus::unresolved
            ? SessionYieldReason::terminal
            : std::chrono::steady_clock::now() >= deadline
                ? SessionYieldReason::deadline
                : recursive_stop.load(std::memory_order_relaxed)
                    ? SessionYieldReason::yield_requested
                    : SessionYieldReason::quantum_complete;
        {
            std::lock_guard lock(event_mutex);
            result.delta = event_delta;
            result.donations = event_donations;
            result.terminal_regions = event_terminal_regions;
            result.workers_used = static_cast<std::size_t>(
                std::count(worker_used.begin(), worker_used.end(), true));
            result.busy_worker_seconds = event_busy_worker_seconds;
        }
        result.allocated_worker_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - service_started).count() *
            static_cast<double>(worker_count);
        // Per-worker intervals and the enclosing wall interval are sampled by
        // different clock calls. On very short epochs timer quantization can
        // make their sum a few ticks larger than wall*workers. Allocation is
        // physically at least the measured busy time; preserve that invariant
        // instead of reporting utilization above 100 percent.
        result.allocated_worker_seconds = std::max(
            result.allocated_worker_seconds, result.busy_worker_seconds);
        if (shared_cache || shared_dynamic_cache) {
            const auto after = shared_cache ? shared_cache->stats()
                                            : shared_dynamic_cache->stats();
            result.delta.failed_cache_queries = after.queries - cache_before.queries;
            result.delta.failed_cache_hits = after.hits - cache_before.hits;
            result.delta.failed_states_recorded = after.inserts - cache_before.inserts;
            result.delta.failed_state_cache_size = after.entries;
            result.delta.failed_state_cache_capacity = after.capacity;
            result.delta.failed_state_cache_memory_bytes = after.memory_bytes;
            result.delta.failed_state_bounds_strengthened = after.strengthenings -
                cache_before.strengthenings;
            result.delta.failed_state_insertions_skipped = after.rejected_capacity -
                cache_before.rejected_capacity;
            result.delta.cache_collisions = after.collisions - cache_before.collisions;
            result.delta.cache_segment_growths =
                after.segment_growths - cache_before.segment_growths;
            result.delta.cache_lookup_probes =
                after.lookup_probes - cache_before.lookup_probes;
            result.delta.cache_insertion_probes =
                after.insertion_probes - cache_before.insertion_probes;
            result.delta.cache_probes_avoided_after_saturation =
                after.probes_avoided_after_saturation -
                cache_before.probes_avoided_after_saturation;
            result.delta.cache_page_promotions = after.page_promotions - cache_before.page_promotions;
            result.delta.cache_page_second_chances = after.page_second_chances - cache_before.page_second_chances;
            result.delta.cache_pages_recycled = after.pages_recycled - cache_before.pages_recycled;
            result.delta.cache_replacement_admissions = after.replacement_admissions - cache_before.replacement_admissions;
            result.delta.cache_entries_evicted = after.entries_evicted - cache_before.entries_evicted;
            result.delta.cache_evicted_depth_sum = after.evicted_depth_sum - cache_before.evicted_depth_sum;
            result.delta.cache_maximum_evicted_depth = after.maximum_evicted_depth;
        }
        result.right_censored = result.status == SessionStatus::unresolved;
        result.configured_proof_regions_bound = options.max_proof_regions;
        result.resolved_proof_regions_bound = resolved_proof_regions_bound;
        result.peak_proof_regions = peak_live_nodes_count.load(std::memory_order_relaxed);
        result.suppressed_donations = suppressed_donations.load(std::memory_order_relaxed);

        result.delta.configured_proof_regions_bound = options.max_proof_regions;
        result.delta.resolved_proof_regions_bound = resolved_proof_regions_bound;
        result.delta.peak_proof_regions = result.peak_proof_regions;
        result.delta.suppressed_donations = result.suppressed_donations;
        return result;
    }

    void cancel() noexcept {
        recursive_stop.store(true, std::memory_order_relaxed);
        std::lock_guard lock(control_mutex);
        if (current_status == SessionStatus::unresolved)
            current_status = SessionStatus::cancelled;
        work_cv.notify_all();
        done_cv.notify_all();
    }

    void request_yield() noexcept {
        recursive_stop.store(true, std::memory_order_relaxed);
        work_cv.notify_all();
        done_cv.notify_all();
    }

    void begin_external_epoch(std::uint64_t work_units,
                              std::chrono::steady_clock::time_point epoch_deadline,
                              std::size_t concurrency_target) {
        if (!external_workers) throw std::logic_error("not an external-worker session");
        std::unique_lock call_lock(service_mutex);
        if (external_epoch_active.load(std::memory_order_acquire))
            throw std::logic_error("external epoch already active");
        {
            std::lock_guard lock(event_mutex);
            event_delta = {};
            event_donations = pending_bootstrap_donations;
            pending_bootstrap_donations = 0;
            event_terminal_regions = 0;
            event_busy_worker_seconds = 0.0;
            worker_used.assign(worker_count, false);
        }
        {
            std::lock_guard lock(control_mutex);
            deadline = epoch_deadline;
            recursive_stop.store(false, std::memory_order_relaxed);
            remaining_work.store(std::max<std::uint64_t>(1, work_units),
                                 std::memory_order_relaxed);
            external_concurrency_target.store(std::max<std::size_t>(1, concurrency_target),
                                              std::memory_order_release);
        }
        external_epoch_active.store(true, std::memory_order_release);
    }

    void begin_external_epoch(std::uint64_t work_units,
                              std::chrono::steady_clock::time_point epoch_deadline) {
        begin_external_epoch(work_units, epoch_deadline, worker_count);
    }

    void begin_external_epoch(const EpochContract& contract) {
        begin_external_epoch(contract.total_work_units, contract.deadline, contract.concurrency_target);
    }

    LeaseOutcome run_external_lease(std::size_t id) {
        if (!external_workers) throw std::logic_error("not an external-worker session");
        if (id >= worker_count) throw std::out_of_range("external worker id out of range");
        {
            std::lock_guard lock(external_mutex);
            if (!external_epoch_active.load(std::memory_order_acquire))
                throw std::logic_error("external epoch is not active");
            ++external_active_leases;
        }
        // Deliberately no service_mutex here: Node, queue, event and control
        // state retain their existing fine-grained synchronization, allowing
        // different external workers to consume distinct proof-forest nodes.
        LeaseOutcome outcome;
        try {
            outcome = worker_epoch(id);
        } catch (...) {
            {
                std::lock_guard lock(exception_mutex);
                if (!service_exception) service_exception = std::current_exception();
            }
            remaining_work.store(0, std::memory_order_relaxed);
            outcome.status = LeaseOutcome::terminal;
        }
        {
            std::lock_guard lock(external_mutex);
            --external_active_leases;
        }
        external_cv.notify_all();
        return outcome;
    }

    ParallelSessionEvent finish_external_epoch() {
        if (!external_workers) throw std::logic_error("not an external-worker session");
        std::unique_lock call_lock(service_mutex);
        {
            std::unique_lock lock(external_mutex);
            external_cv.wait(lock, [this] { return external_active_leases == 0; });
        }
        if (!external_epoch_active.load(std::memory_order_acquire))
            throw std::logic_error("external epoch is not active");
        external_epoch_active.store(false, std::memory_order_release);
        {
            std::lock_guard lock(exception_mutex);
            if (service_exception) std::rethrow_exception(service_exception);
        }
        reconcile_quiescent_frontier();
        ParallelSessionEvent result;
        result.status = current_status_atomic();
        result.reason = result.status != SessionStatus::unresolved
            ? SessionYieldReason::terminal
            : std::chrono::steady_clock::now() >= deadline
                ? SessionYieldReason::deadline : SessionYieldReason::quantum_complete;
        {
            std::lock_guard lock(event_mutex);
            result.delta = event_delta;
            result.donations = event_donations;
            result.terminal_regions = event_terminal_regions;
            result.workers_used = static_cast<std::size_t>(
                std::count(worker_used.begin(), worker_used.end(), true));
            result.busy_worker_seconds = event_busy_worker_seconds;
        }
        result.allocated_worker_seconds = result.busy_worker_seconds;
        result.right_censored = result.status == SessionStatus::unresolved;
        return result;
    }

    const Graph& graph;
    std::uint32_t threshold;
    DecisionOptions options;
    std::size_t worker_count;
    bool external_workers = false;
    mutable std::mutex external_mutex;
    mutable std::condition_variable external_cv;
    std::atomic<bool> external_epoch_active{false};
    std::atomic<std::size_t> external_active_leases{0};
    std::atomic<std::size_t> external_concurrency_target{1};
    std::atomic<std::size_t> steal_reservation_count{0};
    std::shared_ptr<Node> root;
    std::vector<std::shared_ptr<Node>> nodes;
    std::unordered_map<std::uint64_t, std::weak_ptr<Node>> nodes_by_id;
    std::atomic<std::uint64_t> next_node_id{1};
    std::atomic<std::uint64_t> next_reservation_id{1};
    mutable std::mutex nodes_mutex;
    mutable std::mutex queue_mutex;
    std::deque<std::shared_ptr<Node>> ready;
    mutable std::mutex control_mutex;
    std::condition_variable work_cv, done_cv;
    mutable std::mutex service_mutex;
    bool stopping = false, service_active = false;
    std::uint64_t epoch = 0;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
    std::atomic<std::uint64_t> remaining_work{0};
    std::atomic<std::size_t> active_workers{0};
    std::size_t finished_workers = 0; // guarded by control_mutex
    std::size_t epoch_arrivals = 0; // guarded by control_mutex
    bool epoch_start_released = false; // guarded by control_mutex
    std::size_t first_claims_issued = 0; // guarded by control_mutex
    SessionStatus current_status = SessionStatus::unresolved;
    std::vector<Graph::Vertex> solution;
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<RecursiveDynamicSubtreeWorker>> recursive_workers;
    std::atomic<bool> recursive_stop{false};
#ifdef CUTWIDTH_HAVE_ONETBB
    std::unique_ptr<oneapi::tbb::task_arena> one_tbb_arena;
#endif
    std::shared_ptr<ShardedFixedThresholdDynamicCache> shared_cache;
    std::shared_ptr<ShardedDynamicDecisionCache> shared_dynamic_cache;
    std::shared_ptr<CanonicalOwnershipTable> ownership;
    std::optional<MemoryGovernor::Lease> cache_lease;
    std::mutex event_mutex;
    DecisionStats event_delta;
    std::uint64_t event_donations = 0, event_terminal_regions = 0;
    std::uint64_t pending_bootstrap_donations = 0;
    double event_busy_worker_seconds = 0.0;
    std::vector<bool> worker_used;
    std::mutex exception_mutex;
    std::exception_ptr service_exception;
    std::size_t resolved_proof_regions_bound = 0;
    std::atomic<std::size_t> live_nodes_count{0};
    std::atomic<std::size_t> peak_live_nodes_count{0};
    std::atomic<std::uint64_t> suppressed_donations{0};
};

ParallelDecisionSession::ParallelDecisionSession(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    std::size_t workers, bool external_workers)
    : impl_(std::make_unique<Impl>(graph, threshold, std::move(options), workers,
          external_workers)) {}
ParallelDecisionSession::ParallelDecisionSession(
    const Graph& graph, const ParallelDecisionSnapshot& snapshot,
    DecisionOptions options, std::size_t workers, bool external_workers)
    : impl_(std::make_unique<Impl>(
          graph, snapshot, std::move(options), workers, external_workers)) {}
ParallelDecisionSession::~ParallelDecisionSession() = default;
ParallelSessionEvent ParallelDecisionSession::service(const SessionServiceBudget& budget) {
    return impl_->service(budget);
}
void ParallelDecisionSession::request_yield() noexcept { impl_->request_yield(); }
void ParallelDecisionSession::cancel() noexcept { impl_->cancel(); }
SessionStatus ParallelDecisionSession::status() const noexcept {
    return impl_->current_status_atomic();
}
std::vector<Graph::Vertex> ParallelDecisionSession::ordering() const {
    std::lock_guard lock(impl_->control_mutex);
    return impl_->solution;
}
std::size_t ParallelDecisionSession::worker_count() const noexcept {
    return impl_->worker_count;
}
ParallelDecisionSnapshot ParallelDecisionSession::quiesce_and_snapshot(SnapshotPolicy policy) const {
    return impl_->snapshot(policy);
}
std::optional<ParallelUnstartedFragment>
ParallelDecisionSession::inspect_unstarted_fragment() const {
    return impl_->inspect_unstarted_fragment();
}
std::optional<ParallelUnstartedFragment>
ParallelDecisionSession::inspect_deepest_unstarted_fragment() const {
    return impl_->inspect_deepest_unstarted_fragment();
}
bool ParallelDecisionSession::retire_unstarted_fragment(
    const ParallelUnstartedFragment& fragment) {
    return impl_->retire_unstarted_fragment(fragment);
}
std::optional<ParallelUnstartedFragment>
ParallelDecisionSession::claim_unstarted_fragment() {
    return impl_->claim_unstarted_fragment();
}
std::optional<ParallelUnstartedFragment>
ParallelDecisionSession::claim_deepest_unstarted_fragment() {
    return impl_->claim_deepest_unstarted_fragment();
}
std::optional<ParallelUnstartedFragment>
ParallelDecisionSession::donate_and_claim_deepest_unstarted_fragment() {
    return impl_->donate_and_claim_deepest_unstarted_fragment();
}
bool ParallelDecisionSession::retire_claimed_unstarted_fragment(
    const ParallelUnstartedFragment& fragment) {
    return impl_->retire_claimed_unstarted_fragment(fragment);
}
bool ParallelDecisionSession::release_claimed_unstarted_fragment(
    const ParallelUnstartedFragment& fragment) {
    return impl_->release_claimed_unstarted_fragment(fragment);
}
void ParallelDecisionSession::begin_external_epoch(
    std::uint64_t work_units, std::chrono::steady_clock::time_point deadline) {
    impl_->begin_external_epoch(work_units, deadline);
}
void ParallelDecisionSession::begin_external_epoch(const EpochContract& contract) {
    impl_->begin_external_epoch(contract);
}
LeaseOutcome ParallelDecisionSession::run_external_lease(std::size_t worker_id) {
    return impl_->run_external_lease(worker_id);
}
ParallelSessionEvent ParallelDecisionSession::finish_external_epoch() {
    return impl_->finish_external_epoch();
}
bool ParallelDecisionSession::has_runnable_work() const {
    return impl_->has_runnable_work();
}
void ParallelDecisionSession::increment_steal_reservation() {
    impl_->steal_reservation_count.fetch_add(1, std::memory_order_relaxed);
}
void ParallelDecisionSession::decrement_steal_reservation() {
    impl_->steal_reservation_count.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace cutwidth
