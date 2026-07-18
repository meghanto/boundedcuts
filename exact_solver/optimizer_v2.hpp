#pragma once

#include "decision.hpp"
#include "graph.hpp"
#include "milp_adapter.hpp"
#include "pb_backend.hpp"
#include "pb_cadical_incremental.hpp"
#include "memory_governor.hpp"
#include "checkpoint.hpp"

#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cutwidth {

enum class SdpBackend { dense_admm, clarabel, clarabel_bisection };
enum class ProofBackend { dfs, pb };
enum class HeuristicEvaluation { full, incremental };
enum class HeuristicTiebreak { width, cut_profile };
enum class HeuristicSearch { basic, portfolio };
enum class ControllerMode { static_policy, adaptive };
enum class ThresholdSchedulerMode { recurrence, value_aware };

struct MilestoneSnapshot {
    std::uint64_t scheduled_milliseconds = 0;
    std::uint64_t elapsed_milliseconds = 0;
    std::uint32_t lower_bound = 0;
    std::uint32_t upper_bound = 0;
    std::uint64_t nodes_expanded = 0;
    std::uint64_t decision_calls = 0;
    double allocated_worker_seconds = 0.0;
    double busy_worker_seconds = 0.0;
    double controller_overhead_seconds = 0.0;
    bool optimal = false;
};

struct OptimizerV2Options {
    std::chrono::milliseconds time_limit{0};
    bool use_failed_state_cache = true;
    bool use_twin_symmetry = true;
    bool use_depth_two_lookahead = false;
    std::uint32_t depth_two_lookahead_max_remaining = 18;
    bool reuse_failed_state_cache_across_thresholds = true;
    CacheMode cache_mode = CacheMode::automatic;
    // Bounds are opt-in until per-family ablations show a net wall-time gain.
    bool use_partial_bounds = false;
    PartialBoundOptions partial_bounds{};
    DecisionBackend backend = DecisionBackend::automatic;
    ProofBackend proof_backend = ProofBackend::dfs;
    pb::DecisionOptions pb_options{};
    std::size_t threads = 1;
    ControllerMode controller = ControllerMode::static_policy;
    std::vector<std::chrono::milliseconds> milestones{};
    std::size_t memory_budget_bytes = std::size_t{16} * 1024U * 1024U * 1024U;
    // Independent admission ceiling for exact residual subset-DP. Zero is an
    // explicit unlimited cap (the memory governor still has final say).
    std::size_t residual_dp_max_bytes = std::size_t{256} * 1024U * 1024U;
    std::uint32_t dfs_residual_dp_max_remaining = 23;
    std::shared_ptr<MemoryGovernor> memory_governor;
    double controller_overhead_fraction = 0.01;
    std::vector<std::string> adaptive_arms{"bounds", "dfs", "alns", "sdp", "residual-dp"};
    std::optional<std::filesystem::path> checkpoint_out;
    std::optional<std::filesystem::path> resume;
    std::optional<std::filesystem::path> strategy_trace;
    std::size_t parallel_min_cache_shards = 16;
    std::size_t parallel_cache_shards_per_thread = 4;
    ParallelRuntime parallel_runtime = ParallelRuntime::native;
    bool use_canonical_ownership = false;
    bool cooperative_work_stealing = false;
    bool canonical_frontier_bootstrap = false;
    bool recursive_coarse_kernel = false;
    std::size_t annealing_min_vertices = 32;
    std::size_t annealing_iterations_per_vertex = 1000;
    std::size_t annealing_max_iterations = 100000;
    HeuristicEvaluation heuristic_evaluation = HeuristicEvaluation::full;
    HeuristicTiebreak heuristic_tiebreak = HeuristicTiebreak::width;
    // Zero preserves the historical heuristic and its runtime.  Portfolio
    // search is opt-in and receives a separate cap inside the global deadline.
    HeuristicSearch heuristic_search = HeuristicSearch::basic;
    std::chrono::milliseconds heuristic_time{0};
    NodeStateMode node_state = NodeStateMode::recompute;
    NodeOrder node_order = NodeOrder::cut;
    std::uint8_t node_memo_depth = 0;
    std::uint32_t node_memo_max_remaining = 18;
    std::size_t node_memo_memory_bytes = 0;
    unsigned descending_feasible_steps_before_binary = 4;
    double milp_time_seconds = 0.0;
    std::size_t sdp_iterations = 0;
    std::size_t sdp_max_dimension = 256;
    std::size_t sdp_projection_sweeps = 3;
    SdpBackend sdp_backend = SdpBackend::dense_admm;
    double sdp_time_seconds = 0.0;
    std::size_t sdp_max_cone_entries = 12000;
    // Compact fixed-cardinality runs at floor(n/2)-offset. The schedule is
    // consulted only when the SDP prototype is explicitly enabled.
    std::vector<std::size_t> sdp_bisection_offsets{0, 1, 2};
    std::size_t sdp_triangle_cuts = 0;
    unsigned sdp_quantization_bits = 30;
    sdp::SdpSchedule sdp_schedule = sdp::SdpSchedule::off;
    std::chrono::milliseconds sdp_total_time{0};
    std::size_t sdp_max_calls = 0;
    std::size_t sdp_max_state_dimension = 0;
    std::uint64_t sdp_trigger_nodes = 100000;
    std::size_t failed_state_cache_limit = 0;
    std::size_t failed_state_cache_memory_bytes =
        std::size_t{2} * 1024U * 1024U * 1024U;
    CacheReplacementPolicy cache_replacement = CacheReplacementPolicy::freeze;
    std::size_t cache_replacement_page_capacity = std::size_t{1} << 18U;
    std::size_t max_proof_regions = 0;
    bool best_next_buckets = false;
    CandidateEnumerator candidate_enumerator = CandidateEnumerator::scan;
    std::uint32_t local_continuation_depth = 0;
    std::uint32_t local_continuation_max_slack = 1;
    std::uint32_t local_continuation_max_children = 8;
    std::uint64_t local_continuation_max_states = 4096;
    ThresholdSchedulerMode threshold_scheduler = ThresholdSchedulerMode::recurrence;
    std::string pb_sat_root_solver;
    std::string pb_sat_root_checker;
    std::string pb_sat_root_dir;
    std::chrono::milliseconds pb_sat_root_timeout{0};
    std::optional<std::size_t> pb_sat_root_q;
    std::uint32_t pb_sat_root_max_gap = 2;
};

struct OptimizerV2Stats {
    std::uint64_t configured_proof_regions_bound = 0;
    std::uint64_t resolved_proof_regions_bound = 0;
    std::uint64_t peak_proof_regions = 0;
    std::uint64_t suppressed_donations = 0;
    std::uint32_t root_degree_bound = 0;
    std::uint32_t root_density_bound = 0;
    std::uint32_t root_average_degree_bound = 0;
    std::uint32_t root_grooming_bound = 0;
    std::uint64_t decision_calls = 0;
    std::vector<MilestoneSnapshot> milestones;
    std::uint64_t controller_events = 0;
    std::uint64_t censored_decisions = 0;
    double controller_overhead_seconds = 0.0;
    std::uint64_t adaptive_sessions_created = 0;
    std::uint64_t adaptive_session_resumes = 0;
    std::uint64_t adaptive_session_services = 0;
    double adaptive_dfs_service_seconds = 0.0;
    std::size_t adaptive_dfs_worker_allocation = 0;
    std::size_t adaptive_incumbent_worker_allocation = 0;
    double allocated_worker_seconds = 0.0;
    double busy_worker_seconds = 0.0;
    double compatibility_wall_time_capacity_seconds = 0.0;
    std::size_t peak_active_physical_leases = 0;
    std::uint64_t useful_leases = 0;
    std::uint64_t empty_claim_exits = 0;
    std::uint64_t cross_session_steals = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> per_epoch_useful_work;
    std::unordered_map<std::uint32_t, std::uint64_t> per_threshold_useful_work;
    std::uint64_t residual_dp_service_calls = 0;
    std::uint64_t residual_dp_states = 0;
    std::size_t residual_dp_projected_bytes = 0;
    bool residual_dp_applicable = false;
    bool residual_dp_admitted = false;
    std::string residual_dp_skip_reason;
    bool residual_dp_completed = false;
    std::uint64_t residual_dp_attempts = 0;
    std::uint64_t residual_dp_admissions = 0;
    std::uint64_t residual_dp_governor_or_cap_rejections = 0;
    std::uint64_t residual_dp_completed_tails = 0;
    std::uint64_t residual_dp_infeasible_prunes = 0;
    std::uint64_t residual_dp_feasible_witnesses = 0;
    std::uint64_t residual_dp_peak_bytes = 0;
    double residual_dp_seconds = 0.0;
    std::uint64_t residual_dp_cold_restarts = 0;
    MemoryGovernorStats memory{};
    std::uint64_t incumbent_service_calls = 0;
    std::uint64_t incumbent_iterations = 0;
    std::uint64_t incumbent_candidate_evaluations = 0;
    std::uint64_t incumbent_verified_improvements = 0;
    std::uint64_t incumbent_no_progress_bursts = 0;
    double incumbent_service_seconds = 0.0;
    std::size_t parallel_workers_used = 1;
    std::uint64_t parallel_root_tasks_started = 0;
    std::uint64_t parallel_root_tasks_completed = 0;
    std::uint64_t nodes_expanded = 0;
    std::uint64_t children_rejected_by_cut = 0;
    std::uint64_t failed_cache_hits = 0;
    std::uint64_t failed_states_recorded = 0;
    std::uint64_t twin_symmetric_children_skipped = 0;
    std::uint64_t depth_two_lookahead_checks = 0;
    std::uint64_t children_rejected_by_depth_two_lookahead = 0;
    std::uint64_t cache_strengthenings = 0;
    std::uint64_t cache_insertions_skipped = 0;
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
    bool resumed_from_checkpoint = false;
    std::uint64_t checkpoint_elapsed_milliseconds = 0;
    std::uint64_t checkpoints_written = 0;
    double checkpoint_write_seconds = 0.0;
    std::uint64_t checkpoint_reserve_milliseconds = 0;
    std::size_t cache_peak_entries = 0;
    std::size_t cache_peak_capacity = 0;
    std::size_t cache_peak_memory_bytes = 0;
    double cache_bytes_per_state = 0.0;
    bool cache_saturated = false;
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
    std::uint64_t heuristic_interval_evaluations = 0;
    std::uint64_t heuristic_full_fallbacks = 0;
    double heuristic_runtime_seconds = 0.0;
    double time_to_final_upper_bound_seconds = 0.0;
    std::uint64_t heuristic_spectral_seeds = 0;
    std::uint64_t heuristic_grasp_constructions = 0;
    std::uint64_t heuristic_vns_evaluations = 0;
    std::uint64_t heuristic_portfolio_improvements = 0;
    PartialBoundStats partial_bounds{};
    std::uint64_t components_solved = 0;
    bool milp_attempted = false;
    MilpStatus milp_status = MilpStatus::unknown;
    double milp_runtime_seconds = 0.0;
    double milp_model_build_seconds = 0.0;
    double milp_solve_seconds = 0.0;
    std::int64_t milp_nodes = 0;
    std::optional<double> milp_diagnostic_dual_bound;
    bool milp_incumbent_accepted = false;
    bool sdp_attempted = false;
    bool sdp_available = false;
    bool sdp_raw_converged = false;
    double sdp_primal_residual = 0.0;
    std::optional<std::uint32_t> sdp_certified_lower_bound;
    double sdp_primal_objective = 0.0;
    double sdp_dual_objective = 0.0;
    double sdp_dual_residual = 0.0;
    double sdp_solve_seconds = 0.0;
    std::size_t sdp_solver_iterations = 0;
    int sdp_solver_status = -1;
    std::size_t sdp_bisection_calls = 0;
    std::size_t sdp_triangle_cuts = 0;
    std::uint64_t sdp_state_requests = 0;
    std::uint64_t sdp_state_certified = 0;
    std::uint64_t sdp_state_prunes = 0;
    std::uint64_t sdp_state_cache_hits = 0;
    std::uint64_t sdp_state_calls = 0;
    std::uint64_t sdp_state_busy = 0;
    std::uint64_t sdp_state_budget_rejections = 0;
    std::uint64_t sdp_state_uncertified = 0;
    std::uint64_t sdp_state_dimension_rejections = 0;
    std::size_t sdp_state_preferred_max_dimension = 0;
    std::uint64_t pb_calls = 0;
    std::uint64_t pb_sat_certificates = 0;
    std::uint64_t pb_unsat_certificates = 0;
    pb::DecisionProvenance pb_last{};
    bool pb_incremental_attempted = false;
    bool pb_incremental_available = false;
    std::uint64_t pb_incremental_calls = 0;
    std::uint64_t pb_incremental_sat = 0;
    std::uint64_t pb_incremental_unsat_exploratory = 0;
    double pb_incremental_seconds = 0.0;
    std::uint64_t pb_sat_root_attempts = 0;
    std::uint64_t pb_sat_root_sat = 0;
    std::uint64_t pb_sat_root_certified_unsat = 0;
    std::uint64_t pb_sat_root_timeouts = 0;
    std::uint64_t pb_sat_root_failures = 0;
    std::uint64_t pb_sat_root_checker_successes = 0;
    std::uint32_t pb_sat_root_active_threshold = 0;
    std::uint64_t pb_sat_root_active_cardinality = 0;
    double pb_sat_root_solver_seconds = 0.0;
    double pb_sat_root_checker_seconds = 0.0;
    std::string pb_sat_root_last_cnf_path;
    std::string pb_sat_root_last_proof_path;
    std::string pb_sat_root_last_result;
};

struct OptimizerV2Result {
    bool optimal = false;
    std::uint32_t lower_bound = 0;
    std::uint32_t upper_bound = 0;
    std::vector<Graph::Vertex> ordering;
    OptimizerV2Stats stats;
};

[[nodiscard]] OptimizerV2Result optimize_cutwidth_v2(
    const Graph& graph, OptimizerV2Options options = {});
[[nodiscard]] CheckpointCompatibility adaptive_checkpoint_compatibility(
    const Graph& graph, const OptimizerV2Options& options);

// Component-aware threshold decision. A timed-out component yields timed_out
// unless another component has already proved infeasibility.
[[nodiscard]] DecisionResult decide_cutwidth_v2(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options = {});

} // namespace cutwidth
