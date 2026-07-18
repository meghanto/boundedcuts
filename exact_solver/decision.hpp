#pragma once

#include "graph.hpp"
#include "decision_cache.hpp"
#include "partial_bounds.hpp"
#include "sdp_bound_oracle.hpp"
#include "node_memo.hpp"
#include "memory_governor.hpp"
#include "canonical_ownership.hpp"

#include <chrono>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include <memory>

namespace cutwidth {

enum class DecisionStatus {
    feasible,
    infeasible,
    timed_out,
};

enum class DecisionBackend { automatic, word64, dynamic };
enum class CacheMode { automatic, cross_threshold, fixed_threshold };
enum class NodeStateMode { recompute, incremental };
enum class NodeOrder { cut, memo };
enum class ParallelRuntime { native, onetbb };
enum class CandidateEnumerator { scan, delta_buckets, cross_check };
using DecisionTwinTable = std::vector<std::vector<Graph::Vertex>>;

constexpr std::size_t dfs_viable_child_bucket_count = 10;
constexpr std::size_t dfs_slack_bucket_count = 8;

struct DfsBoundDiagnostics {
    std::uint64_t evaluations = 0;
    std::uint64_t prunes = 0;
    std::uint64_t nanoseconds = 0;
};

struct DfsDepthDiagnostics {
    std::uint64_t nodes_entered = 0;
    std::uint64_t cache_queries = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t cache_prunes = 0;
    std::uint64_t sdp_prunes = 0;
    std::uint64_t partial_bound_prunes = 0;
    std::uint64_t residual_dp_prunes = 0;
    std::uint64_t best_next_prunes = 0;
    std::uint64_t dead_ends = 0;
    std::uint64_t children_rejected_by_cut = 0;
    std::uint64_t children_rejected_by_symmetry = 0;
    std::uint64_t children_rejected_by_lookahead = 0;
    std::uint64_t viable_child_observations = 0;
    std::uint64_t viable_children_sum = 0;
    std::array<std::uint64_t, dfs_viable_child_bucket_count>
        viable_child_histogram{};
    std::array<std::uint64_t, dfs_slack_bucket_count> slack_histogram{};
};

struct DfsRootDiagnostics {
    // graph.size() denotes the empty-prefix bucket.
    std::size_t root_vertex = 0;
    std::uint64_t nodes_entered = 0;
    std::uint64_t total_prunes = 0;
    std::uint64_t cache_prunes = 0;
    std::uint64_t bound_prunes = 0;
    std::uint64_t dead_ends = 0;
    std::size_t maximum_depth = 0;
    std::vector<std::uint64_t> nodes_by_depth;
};

struct DfsDiagnostics {
    bool enabled = false;
    std::uint32_t threshold = 0;
    std::uint64_t nodes_entered = 0;
    DfsBoundDiagnostics sdp_bound{};
    DfsBoundDiagnostics partial_bound{};
    DfsBoundDiagnostics residual_dp{};
    DfsBoundDiagnostics best_next_bound{};
    std::vector<DfsDepthDiagnostics> by_depth;
    std::vector<DfsRootDiagnostics> by_root;
};

void merge_dfs_diagnostics(DfsDiagnostics& total, const DfsDiagnostics& part);

[[nodiscard]] std::shared_ptr<const DecisionTwinTable> make_decision_twin_table(
    const Graph& graph, bool enabled);

struct DeltaHistogram {
    std::vector<std::uint32_t> counts;
    std::int32_t offset = 0;
    std::size_t min_idx = 0;

    void init(std::int32_t max_deg) {
        counts.assign(2 * max_deg + 1, 0);
        offset = max_deg;
        min_idx = counts.size();
    }

    void add(std::int32_t delta) {
        std::size_t idx = static_cast<std::size_t>(delta + offset);
        if (idx >= counts.size()) return;
        if (idx < min_idx) {
            min_idx = idx;
        }
        counts[idx]++;
    }

    void remove(std::int32_t delta) {
        std::size_t idx = static_cast<std::size_t>(delta + offset);
        if (idx >= counts.size()) return;
        if (counts[idx] > 0) {
            counts[idx]--;
            if (counts[idx] == 0 && idx == min_idx) {
                while (min_idx < counts.size() && counts[min_idx] == 0) {
                    min_idx++;
                }
            }
        }
    }

    std::int32_t get_min_delta() const {
        if (min_idx >= counts.size()) {
            return std::numeric_limits<std::int32_t>::max();
        }
        return static_cast<std::int32_t>(min_idx) - offset;
    }
};

struct DecisionOptions {
    // Zero means no time limit.
    std::chrono::milliseconds time_limit{0};
    bool use_failed_state_cache = true;
    CacheMode cache_mode = CacheMode::cross_threshold;
    std::shared_ptr<sdp::SdpBoundOracle> sdp_oracle;
    bool use_twin_symmetry = true;
    std::shared_ptr<const DecisionTwinTable> shared_twins;
    // Explore the legal child with the least remaining cut slack first. This
    // is exact branch ordering only: it never rejects a child.
    bool use_fail_first_candidate_order = true;
    // Reject a legal next vertex when it cannot be followed by any legal
    // second vertex. This is exact propagation, but disabled by default until
    // its cost/pruning tradeoff has been benchmarked.
    bool use_depth_two_lookahead = false;
    // Zero means all depths; otherwise evaluate only when this many vertices
    // or fewer remain after the candidate placement.
    std::uint32_t depth_two_lookahead_max_remaining = 18;
    NodeStateMode node_state = NodeStateMode::recompute;
    NodeOrder node_order = NodeOrder::cut;
    std::uint8_t node_memo_depth = 0;
    std::uint32_t node_memo_max_remaining = 18;
    std::size_t node_memo_memory_bytes = 0;
    std::shared_ptr<NodeMemoTable> node_memo;
    bool use_partial_bounds = true;
    PartialBoundOptions partial_bounds{};
    DecisionBackend backend = DecisionBackend::automatic;
    // Root-subtree workers shared by both exact DFS backends.
    std::size_t threads = 1;
    std::size_t parallel_min_cache_shards = 16;
    std::size_t parallel_cache_shards_per_thread = 4;
    ParallelRuntime parallel_runtime = ParallelRuntime::native;
    bool cooperative_work_stealing = false;
    bool canonical_frontier_bootstrap = false;
    // Run complete canonical frontier regions with the classic recursive
    // kernel while retaining only compact task records between epochs.
    bool recursive_coarse_kernel = false;
    // Dimensionless target used to calibrate geometric deadline/yield polling.
    double controller_overhead_fraction = 0.01;
    // Zero means unlimited. A full cache remains readable but stops growing.
    std::size_t failed_state_cache_limit = 0;
    std::size_t failed_state_cache_memory_bytes =
        std::size_t{2} * 1024U * 1024U * 1024U;
    CacheReplacementPolicy cache_replacement = CacheReplacementPolicy::freeze;
    std::size_t cache_replacement_page_capacity = std::size_t{1} << 18U;
    std::shared_ptr<MemoryGovernor> memory_governor;
    std::shared_ptr<ShardedFixedThresholdDynamicCache> shared_fixed_cache;
    std::shared_ptr<ShardedDynamicDecisionCache> shared_dynamic_cache;
    bool use_canonical_ownership = false;
    std::shared_ptr<CanonicalOwnershipTable> canonical_ownership;
    std::uint64_t ownership_id = 0;
    std::size_t max_proof_regions = 0;
    std::uint32_t dfs_residual_dp_max_remaining = 23;
    bool adaptive_residual_dp_arm_applicable = false;
    std::size_t residual_dp_max_bytes = std::size_t{256} * 1024U * 1024U;
    bool best_next_buckets = false;
    CandidateEnumerator candidate_enumerator = CandidateEnumerator::scan;
    // Exact finite-horizon parent propagation. Zero disables it.
    std::uint32_t local_continuation_depth = 0;
    std::uint32_t local_continuation_max_slack = 1;
    std::uint32_t local_continuation_max_children = 8;
    // Zero is unlimited; a reached cap returns "inconclusive", never a prune.
    std::uint64_t local_continuation_max_states = 4096;
    // Audit-only worker-local telemetry. It must not affect search decisions.
    bool collect_dfs_diagnostics = false;
};

struct DecisionStats {
    std::uint64_t configured_proof_regions_bound = 0;
    std::uint64_t resolved_proof_regions_bound = 0;
    std::uint64_t peak_proof_regions = 0;
    std::uint64_t suppressed_donations = 0;
    std::size_t parallel_workers_used = 1;
    std::uint64_t parallel_root_tasks_started = 0;
    std::uint64_t parallel_root_tasks_completed = 0;
    std::uint64_t nodes_expanded = 0;
    std::uint64_t children_rejected_by_cut = 0;
    std::uint64_t failed_cache_hits = 0;
    std::uint64_t failed_cache_queries = 0;
    std::uint64_t failed_states_recorded = 0;
    std::uint64_t twin_symmetric_children_skipped = 0;
    std::uint64_t depth_two_lookahead_checks = 0;
    std::uint64_t children_rejected_by_depth_two_lookahead = 0;
    std::array<std::uint64_t, 5> node_memo_hits_by_depth{};
    std::uint64_t node_memo_computations = 0;
    std::uint64_t node_memo_prunes = 0;
    std::uint64_t node_memo_child_rejections = 0;
    std::uint64_t node_memo_collisions = 0;
    std::uint64_t node_memo_saturation = 0;
    std::size_t node_memo_memory_bytes = 0;
    bool node_memo_available = false;
    std::uint64_t node_state_updates = 0;
    std::uint64_t residual_histogram_updates = 0;
    std::uint64_t node_sorts_avoided = 0;
    std::size_t failed_state_cache_size = 0;
    std::size_t failed_state_cache_capacity = 0;
    std::size_t failed_state_cache_memory_bytes = 0;
    std::uint64_t failed_state_bounds_strengthened = 0;
    std::uint64_t failed_state_insertions_skipped = 0;
    std::uint64_t cache_collisions = 0;
    std::uint64_t cache_segment_growths = 0;
    std::uint64_t cache_lookup_probes = 0;
    std::uint64_t cache_insertion_probes = 0;
    std::uint64_t cache_probes_avoided_after_saturation = 0;
    std::uint64_t cache_page_promotions = 0;
    std::uint64_t cache_page_second_chances = 0;
    std::uint64_t cache_pages_recycled = 0;
    std::uint64_t cache_replacement_admissions = 0;
    std::uint64_t cache_entries_evicted = 0;
    std::uint64_t cache_evicted_depth_sum = 0;
    std::uint32_t cache_maximum_evicted_depth = 0;
    std::uint64_t unique_canonical_claims = 0;
    std::uint64_t duplicate_ownership_waits = 0;
    std::uint64_t ownership_saturation = 0;
    PartialBoundStats partial_bounds{};
    std::uint64_t sdp_requests = 0;
    std::uint64_t sdp_certified = 0;
    std::uint64_t sdp_prunes = 0;
    std::uint64_t residual_dp_attempts = 0;
    std::uint64_t residual_dp_admissions = 0;
    std::uint64_t residual_dp_governor_or_cap_rejections = 0;
    std::uint64_t residual_dp_completed_tails = 0;
    std::uint64_t residual_dp_infeasible_prunes = 0;
    std::uint64_t residual_dp_feasible_witnesses = 0;
    std::uint64_t residual_dp_states = 0;
    std::uint64_t residual_dp_peak_bytes = 0;
    double residual_dp_seconds = 0.0;
    std::uint64_t residual_dp_cold_restarts = 0;
    std::uint32_t dfs_min_remaining_vertices = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t best_next_bucket_checks = 0;
    std::uint64_t best_next_bucket_parent_prunes = 0;
    std::uint64_t best_next_bucket_candidates_avoided = 0;
    std::uint64_t candidate_scan_checks = 0;
    std::uint64_t candidate_index_gathers = 0;
    std::uint64_t candidate_index_bucket_slots_visited = 0;
    std::uint64_t candidate_index_vertices_emitted = 0;
    std::uint64_t candidate_index_forward_updates = 0;
    std::uint64_t candidate_index_rollback_updates = 0;
    std::uint64_t candidate_index_cross_checks = 0;
    std::uint64_t local_continuation_calls = 0;
    std::uint64_t local_continuation_slack_gate_skips = 0;
    std::uint64_t local_continuation_branch_gate_skips = 0;
    std::uint64_t local_continuation_inconclusive = 0;
    std::uint64_t local_continuation_states = 0;
    std::uint64_t local_continuation_parent_prunes = 0;
    std::uint64_t local_continuation_nanoseconds = 0;
    std::uint64_t local_continuation_cross_checks = 0;
    DfsDiagnostics dfs_diagnostics{};
};

struct DecisionResult {
    DecisionStatus status = DecisionStatus::infeasible;
    std::uint32_t threshold = 0;
    // Populated exactly when status == DecisionStatus::feasible.
    std::vector<Graph::Vertex> ordering;
    DecisionStats stats;
};

// Reusable worker-local classic DFS for one complete dynamic-backend subtree.
// The caller owns task persistence and proof-forest accounting; this object
// retains only scratch buffers and immutable graph/threshold acceleration.
class RecursiveDynamicSubtreeWorker {
public:
    RecursiveDynamicSubtreeWorker(
        const Graph& graph, std::uint32_t threshold, DecisionOptions options,
        ShardedDynamicDecisionCache& cache);
    RecursiveDynamicSubtreeWorker(
        const Graph& graph, std::uint32_t threshold, DecisionOptions options,
        ShardedFixedThresholdDynamicCache& cache);
    ~RecursiveDynamicSubtreeWorker();
    RecursiveDynamicSubtreeWorker(RecursiveDynamicSubtreeWorker&&) noexcept;
    RecursiveDynamicSubtreeWorker& operator=(RecursiveDynamicSubtreeWorker&&) noexcept;
    RecursiveDynamicSubtreeWorker(const RecursiveDynamicSubtreeWorker&) = delete;
    RecursiveDynamicSubtreeWorker& operator=(const RecursiveDynamicSubtreeWorker&) = delete;

    [[nodiscard]] DecisionResult run(
        const std::vector<Graph::Vertex>& prefix, std::uint32_t cut,
        const std::atomic<bool>* external_stop = nullptr,
        std::chrono::steady_clock::time_point absolute_deadline =
            std::chrono::steady_clock::time_point::max(),
        std::uint64_t maximum_nodes = std::numeric_limits<std::uint64_t>::max());

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Decides whether graph has a vertex ordering of cutwidth at most threshold.
// An infeasible result is a proof; a timed_out result makes no feasibility claim.
[[nodiscard]] DecisionResult decide_cutwidth(
    const Graph& graph,
    std::uint32_t threshold,
    DecisionOptions options = {});

// Shared-cache overloads used by optimization across multiple thresholds.
[[nodiscard]] DecisionResult decide_cutwidth_cached(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    Word64DecisionCache& cache);
[[nodiscard]] DecisionResult decide_cutwidth_cached(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    DynamicDecisionCache& cache);
[[nodiscard]] DecisionResult decide_cutwidth_cached(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    ShardedDynamicDecisionCache& cache);
[[nodiscard]] DecisionResult decide_cutwidth_cached(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    FixedThresholdWord64Cache& cache);

} // namespace cutwidth
