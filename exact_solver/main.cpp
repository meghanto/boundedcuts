#include "graph.hpp"
#include "optimizer_v2.hpp"
#include "parallel_decision_session.hpp"
#include "solver.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct CliOptions {
    std::optional<std::string> input;
    std::chrono::milliseconds time_limit{0};
    std::optional<std::uint32_t> max_width;
    bool persistent_decision_session = false;
    bool recursive_coarse_kernel = false;
    std::string engine = "v3";
    cutwidth::DecisionBackend backend = cutwidth::DecisionBackend::automatic;
    cutwidth::CacheMode cache_mode = cutwidth::CacheMode::automatic;
    cutwidth::ProofBackend proof_backend = cutwidth::ProofBackend::dfs;
    cutwidth::pb::SolverKind pb_solver = cutwidth::pb::SolverKind::kissat;
    cutwidth::pb::CardinalityEncoding pb_encoding =
        cutwidth::pb::CardinalityEncoding::sequential_counter;
    std::string pb_solver_path;
    std::string pb_proof_checker;
    std::string pb_expected_version;
    std::chrono::milliseconds pb_proof_check_time{0};
    bool pb_keep_files = false;
    bool pb_channel_positions = false;
    bool pb_split_first = false;
    bool pb_native_incremental = false;
    std::string pb_sat_root_solver;
    std::string pb_sat_root_checker;
    std::string pb_sat_root_dir;
    cutwidth::PbSatRootBackend pb_sat_root_backend =
        cutwidth::PbSatRootBackend::embedded;
    std::string pb_sat_root_ordering = "auto";
    std::chrono::milliseconds pb_sat_root_timeout{0};
    std::optional<std::size_t> pb_sat_root_q;
    std::uint32_t pb_sat_root_max_gap = 2;
    std::size_t cache_memory_bytes = std::size_t{2} * 1024U * 1024U * 1024U;
    cutwidth::CacheReplacementPolicy cache_replacement =
        cutwidth::CacheReplacementPolicy::freeze;
    std::size_t cache_replacement_page_capacity = std::size_t{1} << 18U;
    cutwidth::NodeStateMode node_state = cutwidth::NodeStateMode::recompute;
    cutwidth::NodeOrder node_order = cutwidth::NodeOrder::cut;
    std::uint8_t node_memo_depth = 0;
    std::uint32_t node_memo_max_remaining = 18;
    std::size_t node_memo_memory_bytes = 0;
    std::uint32_t partial_bound_max_slack = 1;
    cutwidth::HeuristicEvaluation heuristic_evaluation = cutwidth::HeuristicEvaluation::full;
    cutwidth::HeuristicTiebreak heuristic_tiebreak = cutwidth::HeuristicTiebreak::width;
    cutwidth::HeuristicSearch heuristic_search = cutwidth::HeuristicSearch::basic;
    std::chrono::milliseconds heuristic_time{0};
    std::size_t threads = 1;
    cutwidth::ControllerMode controller = cutwidth::ControllerMode::static_policy;
    cutwidth::ThresholdSchedulerMode threshold_scheduler = cutwidth::ThresholdSchedulerMode::recurrence;
    std::vector<std::chrono::milliseconds> milestones;
    std::size_t memory_budget_bytes = std::size_t{16} * 1024U * 1024U * 1024U;
    std::size_t residual_dp_max_bytes = std::size_t{256} * 1024U * 1024U;
    std::uint32_t dfs_residual_dp_max_remaining = 23;
    double controller_overhead_fraction = 0.01;
    std::vector<std::string> adaptive_arms{"bounds", "dfs", "alns", "sdp", "residual-dp"};
    std::optional<std::filesystem::path> checkpoint_out;
    std::optional<std::filesystem::path> resume;
    std::optional<std::filesystem::path> strategy_trace;
    std::optional<std::filesystem::path> dfs_diagnostics;
    std::chrono::milliseconds milp_time{0};
    std::uint32_t lookahead_max_remaining = 18;
    std::size_t parallel_min_cache_shards = 16;
    std::size_t parallel_cache_shards_per_thread = 4;
    cutwidth::ParallelRuntime parallel_runtime = cutwidth::ParallelRuntime::native;
    bool use_canonical_ownership = false;
    bool cooperative_work_stealing = false;
    bool canonical_frontier_bootstrap = false;
    std::size_t annealing_min_vertices = 32;
    unsigned descending_feasible_steps = 4;
    std::size_t sdp_iterations = 0;
    std::size_t sdp_max_dimension = 256;
    cutwidth::SdpBackend sdp_backend = cutwidth::SdpBackend::dense_admm;
    std::chrono::milliseconds sdp_time{0};
    std::size_t sdp_max_cone_entries = 12000;
    std::vector<std::size_t> sdp_bisection_offsets{0, 1, 2};
    std::size_t sdp_triangle_cuts = 0;
    unsigned sdp_quantization_bits = 30;
    cutwidth::sdp::SdpSchedule sdp_schedule = cutwidth::sdp::SdpSchedule::off;
    std::chrono::milliseconds sdp_total_time{0};
    std::size_t sdp_max_calls = 0;
    std::size_t sdp_max_state_dimension = 0;
    std::uint64_t sdp_trigger_nodes = 100000;
    std::optional<std::string> features;
    std::size_t max_proof_regions = 0;
    bool best_next_buckets = false;
    cutwidth::CandidateEnumerator candidate_enumerator =
        cutwidth::CandidateEnumerator::scan;
    std::uint32_t local_continuation_depth = 0;
    std::uint32_t local_continuation_max_slack = 1;
    std::uint32_t local_continuation_max_children = 8;
    std::uint64_t local_continuation_max_states = 4096;
    bool lagrangian_prefix_bound = false;
    std::uint32_t lagrangian_max_slack = 1;
    std::uint32_t lagrangian_max_residual = 256;
    std::uint32_t lagrangian_denominator = 2;
    bool json = false;
    bool verify = false;
    bool help = false;
};

constexpr std::string_view usage = R"(Usage: cutwidth_exact [OPTIONS] [FILE]

Solve an undirected cutwidth instance exactly when the search completes.
With no FILE, input is read from standard input.

Options:
  -i, --input FILE          Read the edge list from FILE
  -t, --time-limit SECONDS  Wall-clock limit; 0 means unlimited
  -k, --max-width K         Decide whether cutwidth is at most K
      --legacy              Use the original optimization engine
      --engine NAME         Select v3, v2, or legacy
      --backend NAME        Select auto, word64, or dynamic
      --cache-memory BYTES  Failed-state cache memory budget; 0 is unlimited
      --cache-mode MODE     Select auto, cross-threshold, or fixed-threshold
      --cache-replacement MODE  Select freeze or generational-clock; default freeze
      --cache-replacement-page-capacity N  Power-of-two page cap; default 262144
      --decision-session MODE  Select classic or persistent for --max-width
      --persistent-kernel MODE Select stack or coarse-recursive persistent DFS
      --node-state MODE     Select recompute or incremental
      --node-order MODE     Select cut or memo
      --node-memo-depth N   Finite horizon 0..4; 0 disables the node oracle
      --node-memo-max-remaining N  Residual-size oracle gate; default 18
      --node-memo-memory BYTES  Memo share of the global cache budget
      --partial-bound-max-slack N  Run expensive partial bounds only at slack <= N
      --lagrangian-prefix-bound     Enable Lagrangian prefix bound (default off)
      --lagrangian-max-slack N      Run Lagrangian bound only at slack <= N (default 1)
      --lagrangian-max-residual N   Run Lagrangian bound only with residual size <= N (default 256)
      --lagrangian-denominator N    Lagrangian step multiplier denominator (default 2, positive)
      --heuristic-eval MODE Select full or incremental layout evaluation
      --heuristic-tiebreak MODE  Select width or cut-profile
      --heuristic-search MODE  Select basic or spectral-grasp portfolio
      --heuristic-time SECONDS  Portfolio budget inside the global limit
      --proof-backend NAME  Select dfs or certified external pb
      --pb-solver NAME      Select kissat or cadical
      --pb-encoding NAME    Select sequential or totalizer
      --pb-solver-path FILE Absolute path to the pinned SAT executable
      --pb-proof-checker FILE  Absolute path to a DRAT proof checker
      --pb-expected-version TEXT  Required substring of solver --version
      --pb-proof-check-time SECONDS  Reserve time for independent UNSAT checking
      --pb-keep-files       Retain private CNF, proof, and solver logs
      --pb-channel-positions  Add a channelled permutation/order model
      --pb-split-first      Partition proof by fixed first vertex
      --pb-native-incremental  Use the pinned in-process CaDiCaL decision path
      --pb-sat-root-solver FILE     Absolute path to proof-producing SAT solver
      --pb-sat-root-checker FILE    Absolute path to DRAT proof checker
      --pb-sat-root-dir DIR         Artifact directory for pb-sat-root job
      --pb-sat-root-backend NAME    Select embedded or external (default embedded)
      --pb-sat-root-ordering NAME   PB-only ordering: auto, identity, or rcm (default auto)
      --pb-sat-root-timeout SECONDS Timeout for pb-sat-root solver and checker runs
      --pb-sat-root-q N             Cardinality q for pb-sat-root (default: n/2)
      --pb-sat-root-max-gap N       Start root proof when U-L <= N (default: 2)
      --threads N           Root-search worker threads; default 1
      --controller MODE     Select static or adaptive; default static
      --threshold-scheduler MODE  Select recurrence, value-aware, or primary-first; default recurrence
      --milestones LIST     Comma-separated snapshot seconds
      --memory-budget BYTES Total declared memory budget; default 16 GiB
      --residual-dp-max-bytes BYTES  Residual subset-DP admission ceiling; default 256 MiB (0 unlimited)
      --dfs-residual-dp-max-remaining N  DFS tail residual-DP threshold remaining limit; default 23 (0 disabled)
      --controller-overhead-fraction F  Dimensionless target; default 0.01
      --adaptive-arms LIST  bounds,dfs,alns,sdp,residual-dp,pb-sat-root (dfs required; others ablatable)
      --checkpoint-out FILE Atomically write adaptive continuation state on timeout (post-budget)
      --resume FILE         Resume a compatible adaptive checkpoint
      --strategy-trace FILE Append scheduler events as JSON Lines
      --dfs-diagnostics FILE  Write audit-only DFS depth/root diagnostics JSON
      --milp-time SECONDS   Optional native HiGHS root oracle; 0 disables it
      --lookahead-remaining N  Depth-two lookahead residual-size gate
      --cache-min-shards N  Minimum shared-cache shards; default 16
      --cache-shards-per-thread N  Shared-cache shard multiplier; default 4
      --parallel-runtime NAME Select native or legacy/optional oneTBB executor (not recommended)
      --canonical-ownership MODE  Select on or off for persistent DFS
      --best-next-buckets MODE    Select on or off for best-next bucket optimization; default off
      --candidate-enumerator MODE Select scan, delta-buckets, or cross-check; default scan
      --local-continuation-depth N  Exact finite-horizon probe; 0 disables (default)
      --local-continuation-max-slack N  Admit probe only at cut slack <= N; default 1
      --local-continuation-max-children N  Admit probe only with <= N children; default 8
      --local-continuation-max-states N  Probe state cap; 0 unlimited, default 4096
      --cooperative-stealing MODE Select on or off for safe-point work stealing
      --canonical-frontier MODE Select on or off for subset-deduplicated bootstrap
      --max-proof-regions N Explicit bounded proof-forest region limit; 0 means auto
      --anneal-min-vertices N  Minimum graph size for annealing; default 32
      --descending-steps N  Feasible descending probes before binary search
      --sdp-iterations N    Experimental root SDP iterations; 0 disables it
      --sdp-max-dimension N Experimental dense SDP dimension cap
      --sdp-backend NAME   Select admm, clarabel, or clarabel-bisection
      --sdp-time SECONDS   Optional root SDP time limit
      --sdp-max-cone-entries N  Clarabel PSD triangle entry cap
      --sdp-bisection-offsets LIST  Half-cardinality offsets; default 0,1,2
      --sdp-triangle-cuts N  Add one round of at most N violated triangle cuts
      --sdp-quantization-bits N  Certificate dyadic precision; default 30
      --sdp-schedule MODE  Select off, root, or adaptive partial-state SDP
      --sdp-total-time SECONDS  Global partial-state SDP wall budget
      --sdp-max-calls N    Maximum partial-state backend calls; 0 is unlimited
      --sdp-max-state-dimension N  Maximum partial-state moment dimension
      --sdp-trigger-nodes N  Generic adaptive cache-miss node trigger
      --features LIST       Exact feature set: none, all, or comma-separated
      --json                Emit machine-readable JSON
      --verify              Recompute and check the returned ordering
  -h, --help                Show this help
)";

[[nodiscard]] std::chrono::milliseconds parse_seconds(const std::string& text) {
    std::size_t used = 0;
    double seconds = 0.0;
    try {
        seconds = std::stod(text, &used);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid time limit: " + text);
    }
    if (used != text.size() || !std::isfinite(seconds) || seconds < 0.0) {
        throw std::invalid_argument("invalid time limit: " + text);
    }
    constexpr double max_ms = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    if (seconds * 1000.0 > max_ms) {
        throw std::invalid_argument("time limit is too large");
    }
    if (seconds == 0.0) return std::chrono::milliseconds{0};
    // A positive duration must never truncate to the zero sentinel, which
    // means "unlimited" to the solver.
    return std::chrono::milliseconds(std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::ceil(seconds * 1000.0))));
}

[[nodiscard]] std::uint32_t parse_width(const std::string& text) {
    std::size_t used = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &used);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid maximum width: " + text);
    }
    if (used != text.size() || value > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("invalid maximum width: " + text);
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::size_t parse_size(const std::string& text, std::string_view label) {
    std::size_t used = 0;
    unsigned long long value = 0;
    try { value = std::stoull(text, &used); }
    catch (const std::exception&) { throw std::invalid_argument("invalid " + std::string(label) + ": " + text); }
    if (used != text.size() || value > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("invalid " + std::string(label) + ": " + text);
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::vector<std::size_t> parse_size_list(
    const std::string& text, std::string_view label) {
    if (text.empty()) throw std::invalid_argument(std::string(label) + " must not be empty");
    std::vector<std::size_t> values;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto comma = text.find(',', begin);
        const auto token = text.substr(begin, comma == std::string::npos ? comma : comma - begin);
        if (token.empty()) throw std::invalid_argument("invalid " + std::string(label) + ": " + text);
        const auto value = parse_size(token, label);
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return values;
}

[[nodiscard]] std::vector<std::string> parse_string_list(const std::string& text) {
    if (text.empty()) throw std::invalid_argument("list must not be empty");
    std::vector<std::string> values;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto comma = text.find(',', begin);
        auto token = text.substr(begin, comma == std::string::npos ? comma : comma - begin);
        if (token.empty()) throw std::invalid_argument("invalid empty list item");
        if (std::find(values.begin(), values.end(), token) == values.end())
            values.push_back(std::move(token));
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return values;
}

[[nodiscard]] CliOptions parse_args(int argc, char** argv) {
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value_after = [&](std::string_view option) -> std::string {
            if (++i >= argc) {
                throw std::invalid_argument(std::string(option) + " requires a value");
            }
            return argv[i];
        };
        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "--json") {
            options.json = true;
        } else if (arg == "--verify") {
            options.verify = true;
        } else if (arg == "--legacy") {
            options.engine = "legacy";
        } else if (arg == "--engine") {
            options.engine = value_after(arg);
            if (options.engine != "v3" && options.engine != "v2" && options.engine != "legacy")
                throw std::invalid_argument("engine must be v3, v2, or legacy");
        } else if (arg == "--backend") {
            const auto value = value_after(arg);
            if (value == "auto") options.backend = cutwidth::DecisionBackend::automatic;
            else if (value == "word64") options.backend = cutwidth::DecisionBackend::word64;
            else if (value == "dynamic") options.backend = cutwidth::DecisionBackend::dynamic;
            else throw std::invalid_argument("backend must be auto, word64, or dynamic");
        } else if (arg == "--cache-memory") {
            options.cache_memory_bytes = parse_size(value_after(arg), "cache memory");
        } else if (arg == "--cache-mode") {
            const auto value = value_after(arg);
            if (value == "auto") options.cache_mode = cutwidth::CacheMode::automatic;
            else if (value == "cross-threshold")
                options.cache_mode = cutwidth::CacheMode::cross_threshold;
            else if (value == "fixed-threshold")
                options.cache_mode = cutwidth::CacheMode::fixed_threshold;
            else throw std::invalid_argument(
                "cache mode must be auto, cross-threshold, or fixed-threshold");
        } else if (arg == "--cache-replacement") {
            const auto value = value_after(arg);
            if (value == "freeze")
                options.cache_replacement = cutwidth::CacheReplacementPolicy::freeze;
            else if (value == "generational-clock")
                options.cache_replacement =
                    cutwidth::CacheReplacementPolicy::generational_clock;
            else throw std::invalid_argument(
                "cache replacement must be freeze or generational-clock");
        } else if (arg == "--cache-replacement-page-capacity") {
            options.cache_replacement_page_capacity = parse_size(
                value_after(arg), "cache replacement page capacity");
        } else if (arg == "--decision-session") {
            const auto value = value_after(arg);
            if (value == "classic") options.persistent_decision_session = false;
            else if (value == "persistent") options.persistent_decision_session = true;
            else throw std::invalid_argument(
                "decision session must be classic or persistent");
        } else if (arg == "--persistent-kernel") {
            const auto value = value_after(arg);
            if (value == "stack") options.recursive_coarse_kernel = false;
            else if (value == "coarse-recursive")
                options.recursive_coarse_kernel = true;
            else throw std::invalid_argument(
                "persistent kernel must be stack or coarse-recursive");
        } else if (arg == "--node-state") {
            const auto value = value_after(arg);
            if (value == "recompute") options.node_state = cutwidth::NodeStateMode::recompute;
            else if (value == "incremental") options.node_state = cutwidth::NodeStateMode::incremental;
            else throw std::invalid_argument("node state must be recompute or incremental");
        } else if (arg == "--node-order") {
            const auto value = value_after(arg);
            if (value == "cut") options.node_order = cutwidth::NodeOrder::cut;
            else if (value == "memo") options.node_order = cutwidth::NodeOrder::memo;
            else throw std::invalid_argument("node order must be cut or memo");
        } else if (arg == "--node-memo-depth") {
            const auto value = parse_size(value_after(arg), "node memo depth");
            if (value > 4) throw std::invalid_argument("node memo depth must be between 0 and 4");
            options.node_memo_depth = static_cast<std::uint8_t>(value);
        } else if (arg == "--node-memo-max-remaining") {
            const auto value = parse_size(value_after(arg), "node memo maximum remaining");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("node memo maximum remaining is too large");
            options.node_memo_max_remaining = static_cast<std::uint32_t>(value);
        } else if (arg == "--node-memo-memory") {
            options.node_memo_memory_bytes = parse_size(value_after(arg), "node memo memory");
        } else if (arg == "--partial-bound-max-slack") {
            const auto value = parse_size(value_after(arg), "partial bound maximum slack");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("partial bound maximum slack is too large");
            options.partial_bound_max_slack = static_cast<std::uint32_t>(value);
        } else if (arg == "--lagrangian-prefix-bound") {
            options.lagrangian_prefix_bound = true;
        } else if (arg == "--lagrangian-max-slack") {
            const auto value = parse_size(value_after(arg), "lagrangian maximum slack");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("lagrangian maximum slack is too large");
            options.lagrangian_max_slack = static_cast<std::uint32_t>(value);
        } else if (arg == "--lagrangian-max-residual") {
            const auto value = parse_size(value_after(arg), "lagrangian maximum residual");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("lagrangian maximum residual is too large");
            options.lagrangian_max_residual = static_cast<std::uint32_t>(value);
        } else if (arg == "--lagrangian-denominator") {
            const auto value = parse_size(value_after(arg), "lagrangian denominator");
            if (value == 0 || value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("lagrangian denominator must be positive and fit in 32-bit uint");
            options.lagrangian_denominator = static_cast<std::uint32_t>(value);
        } else if (arg == "--heuristic-eval") {
            const auto value = value_after(arg);
            if (value == "full") options.heuristic_evaluation = cutwidth::HeuristicEvaluation::full;
            else if (value == "incremental") options.heuristic_evaluation = cutwidth::HeuristicEvaluation::incremental;
            else throw std::invalid_argument("heuristic evaluation must be full or incremental");
        } else if (arg == "--heuristic-tiebreak") {
            const auto value = value_after(arg);
            if (value == "width") options.heuristic_tiebreak = cutwidth::HeuristicTiebreak::width;
            else if (value == "cut-profile") options.heuristic_tiebreak = cutwidth::HeuristicTiebreak::cut_profile;
            else throw std::invalid_argument("heuristic tiebreak must be width or cut-profile");
        } else if (arg == "--heuristic-search") {
            const auto value = value_after(arg);
            if (value == "basic") options.heuristic_search = cutwidth::HeuristicSearch::basic;
            else if (value == "portfolio")
                options.heuristic_search = cutwidth::HeuristicSearch::portfolio;
            else throw std::invalid_argument("heuristic search must be basic or portfolio");
        } else if (arg == "--heuristic-time") {
            options.heuristic_time = parse_seconds(value_after(arg));
        } else if (arg == "--proof-backend") {
            const auto value = value_after(arg);
            if (value == "dfs") options.proof_backend = cutwidth::ProofBackend::dfs;
            else if (value == "pb") options.proof_backend = cutwidth::ProofBackend::pb;
            else throw std::invalid_argument("proof backend must be dfs or pb");
        } else if (arg == "--pb-solver") {
            const auto value = value_after(arg);
            if (value == "kissat") options.pb_solver = cutwidth::pb::SolverKind::kissat;
            else if (value == "cadical") options.pb_solver = cutwidth::pb::SolverKind::cadical;
            else throw std::invalid_argument("PB solver must be kissat or cadical");
        } else if (arg == "--pb-encoding") {
            const auto value = value_after(arg);
            if (value == "sequential")
                options.pb_encoding = cutwidth::pb::CardinalityEncoding::sequential_counter;
            else if (value == "totalizer")
                options.pb_encoding = cutwidth::pb::CardinalityEncoding::totalizer;
            else throw std::invalid_argument("PB encoding must be sequential or totalizer");
        } else if (arg == "--pb-solver-path") {
            options.pb_solver_path = value_after(arg);
        } else if (arg == "--pb-proof-checker") {
            options.pb_proof_checker = value_after(arg);
        } else if (arg == "--pb-expected-version") {
            options.pb_expected_version = value_after(arg);
        } else if (arg == "--pb-proof-check-time") {
            options.pb_proof_check_time = parse_seconds(value_after(arg));
        } else if (arg == "--pb-keep-files") {
            options.pb_keep_files = true;
        } else if (arg == "--pb-channel-positions") {
            options.pb_channel_positions = true;
        } else if (arg == "--pb-split-first") {
            options.pb_split_first = true;
        } else if (arg == "--pb-native-incremental") {
            options.pb_native_incremental = true;
        } else if (arg == "--pb-sat-root-solver") {
            options.pb_sat_root_solver = value_after(arg);
        } else if (arg == "--pb-sat-root-checker") {
            options.pb_sat_root_checker = value_after(arg);
        } else if (arg == "--pb-sat-root-dir") {
            options.pb_sat_root_dir = value_after(arg);
        } else if (arg == "--pb-sat-root-backend") {
            const auto value = value_after(arg);
            if (value == "embedded")
                options.pb_sat_root_backend = cutwidth::PbSatRootBackend::embedded;
            else if (value == "external")
                options.pb_sat_root_backend = cutwidth::PbSatRootBackend::external;
            else
                throw std::invalid_argument(
                    "pb-sat-root backend must be embedded or external");
        } else if (arg == "--pb-sat-root-timeout") {
            options.pb_sat_root_timeout = parse_seconds(value_after(arg));
        } else if (arg == "--pb-sat-root-ordering") {
            options.pb_sat_root_ordering = value_after(arg);
            if (options.pb_sat_root_ordering != "auto" &&
                options.pb_sat_root_ordering != "identity" &&
                options.pb_sat_root_ordering != "rcm")
                throw std::invalid_argument(
                    "pb-sat-root ordering must be auto, identity, or rcm");
        } else if (arg == "--pb-sat-root-q") {
            options.pb_sat_root_q = parse_size(value_after(arg), "pb-sat-root q");
        } else if (arg == "--pb-sat-root-max-gap") {
            const auto value = parse_size(value_after(arg), "pb-sat-root maximum gap");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("pb-sat-root maximum gap is too large");
            options.pb_sat_root_max_gap = static_cast<std::uint32_t>(value);
        } else if (arg == "--threads") {
            options.threads = parse_size(value_after(arg), "thread count");
            if (options.threads == 0) throw std::invalid_argument("thread count must be positive");
        } else if (arg == "--controller") {
            const auto value = value_after(arg);
            if (value == "static") options.controller = cutwidth::ControllerMode::static_policy;
            else if (value == "adaptive") options.controller = cutwidth::ControllerMode::adaptive;
            else throw std::invalid_argument("controller must be static or adaptive");
        } else if (arg == "--threshold-scheduler") {
            const auto value = value_after(arg);
            if (value == "recurrence") options.threshold_scheduler = cutwidth::ThresholdSchedulerMode::recurrence;
            else if (value == "value" || value == "value-aware") options.threshold_scheduler = cutwidth::ThresholdSchedulerMode::value_aware;
            else if (value == "primary" || value == "primary-first") options.threshold_scheduler = cutwidth::ThresholdSchedulerMode::primary_first;
            else throw std::invalid_argument("threshold-scheduler must be recurrence, value-aware, or primary-first");
        } else if (arg == "--milestones") {
            options.milestones.clear();
            for (const auto seconds : parse_size_list(value_after(arg), "milestones")) {
                if (seconds == 0 || seconds > static_cast<std::size_t>(
                        std::numeric_limits<std::int64_t>::max() / 1000))
                    throw std::invalid_argument("milestones must be positive and finite");
                options.milestones.emplace_back(static_cast<std::int64_t>(seconds * 1000));
            }
            if (!std::is_sorted(options.milestones.begin(), options.milestones.end()))
                throw std::invalid_argument("milestones must be increasing");
        } else if (arg == "--memory-budget") {
            options.memory_budget_bytes = parse_size(value_after(arg), "memory budget");
        } else if (arg == "--residual-dp-max-bytes") {
            options.residual_dp_max_bytes = parse_size(
                value_after(arg), "residual DP maximum bytes");
        } else if (arg == "--dfs-residual-dp-max-remaining") {
            const auto value = parse_size(value_after(arg), "DFS residual DP maximum remaining");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("DFS residual DP maximum remaining is too large");
            options.dfs_residual_dp_max_remaining = static_cast<std::uint32_t>(value);
        } else if (arg == "--controller-overhead-fraction") {
            const auto value = value_after(arg);
            std::size_t used = 0;
            options.controller_overhead_fraction = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(options.controller_overhead_fraction) ||
                options.controller_overhead_fraction <= 0.0 ||
                options.controller_overhead_fraction >= 1.0)
                throw std::invalid_argument("controller overhead fraction must be between 0 and 1");
        } else if (arg == "--adaptive-arms") {
            options.adaptive_arms = parse_string_list(value_after(arg));
            for (const auto& arm : options.adaptive_arms)
                if (arm != "bounds" && arm != "dfs" && arm != "alns" &&
                    arm != "sdp" && arm != "residual-dp" && arm != "pb-sat-root")
                    throw std::invalid_argument("unknown adaptive arm: " + arm);
        } else if (arg == "--checkpoint-out") {
            options.checkpoint_out = value_after(arg);
            if (options.checkpoint_out->empty())
                throw std::invalid_argument("checkpoint output path must not be empty");
        } else if (arg == "--resume") {
            options.resume = value_after(arg);
            if (options.resume->empty())
                throw std::invalid_argument("resume path must not be empty");
        } else if (arg == "--strategy-trace") {
            options.strategy_trace = value_after(arg);
            if (options.strategy_trace->empty())
                throw std::invalid_argument("strategy trace path must not be empty");
        } else if (arg == "--dfs-diagnostics") {
            options.dfs_diagnostics = value_after(arg);
            if (options.dfs_diagnostics->empty())
                throw std::invalid_argument("DFS diagnostics path must not be empty");
        } else if (arg == "--milp-time") {
            options.milp_time = parse_seconds(value_after(arg));
        } else if (arg == "--lookahead-remaining") {
            const auto value = parse_size(value_after(arg), "lookahead remaining");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("lookahead remaining is too large");
            options.lookahead_max_remaining = static_cast<std::uint32_t>(value);
        } else if (arg == "--cache-min-shards") {
            options.parallel_min_cache_shards = parse_size(value_after(arg), "minimum cache shards");
            if (options.parallel_min_cache_shards == 0)
                throw std::invalid_argument("minimum cache shards must be positive");
        } else if (arg == "--cache-shards-per-thread") {
            options.parallel_cache_shards_per_thread = parse_size(value_after(arg), "cache shards per thread");
            if (options.parallel_cache_shards_per_thread == 0)
                throw std::invalid_argument("cache shards per thread must be positive");
        } else if (arg == "--parallel-runtime") {
            const auto value = value_after(arg);
            if (value == "native") options.parallel_runtime = cutwidth::ParallelRuntime::native;
            else if (value == "onetbb") options.parallel_runtime = cutwidth::ParallelRuntime::onetbb;
            else throw std::invalid_argument("parallel runtime must be native or onetbb");
        } else if (arg == "--canonical-ownership") {
            const auto value = value_after(arg);
            if (value == "on") options.use_canonical_ownership = true;
            else if (value == "off") options.use_canonical_ownership = false;
            else throw std::invalid_argument("canonical ownership must be on or off");
        } else if (arg == "--best-next-buckets") {
            const auto value = value_after(arg);
            if (value == "on") options.best_next_buckets = true;
            else if (value == "off") options.best_next_buckets = false;
            else throw std::invalid_argument("best next buckets must be on or off");
        } else if (arg == "--candidate-enumerator") {
            const auto value = value_after(arg);
            if (value == "scan")
                options.candidate_enumerator = cutwidth::CandidateEnumerator::scan;
            else if (value == "delta-buckets")
                options.candidate_enumerator = cutwidth::CandidateEnumerator::delta_buckets;
            else if (value == "cross-check")
                options.candidate_enumerator = cutwidth::CandidateEnumerator::cross_check;
            else
                throw std::invalid_argument(
                    "candidate enumerator must be scan, delta-buckets, or cross-check");
        } else if (arg == "--local-continuation-depth") {
            const auto value = parse_size(value_after(arg), "local continuation depth");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("local continuation depth is too large");
            options.local_continuation_depth = static_cast<std::uint32_t>(value);
        } else if (arg == "--local-continuation-max-slack") {
            const auto value = parse_size(value_after(arg), "local continuation slack");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("local continuation slack is too large");
            options.local_continuation_max_slack = static_cast<std::uint32_t>(value);
        } else if (arg == "--local-continuation-max-children") {
            const auto value = parse_size(value_after(arg), "local continuation children");
            if (value > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("local continuation child cap is too large");
            options.local_continuation_max_children = static_cast<std::uint32_t>(value);
        } else if (arg == "--local-continuation-max-states") {
            options.local_continuation_max_states = parse_size(
                value_after(arg), "local continuation states");
        } else if (arg == "--cooperative-stealing") {
            const auto value = value_after(arg);
            if (value == "on") options.cooperative_work_stealing = true;
            else if (value == "off") options.cooperative_work_stealing = false;
            else throw std::invalid_argument("cooperative stealing must be on or off");
        } else if (arg == "--canonical-frontier") {
            const auto value = value_after(arg);
            if (value == "on") options.canonical_frontier_bootstrap = true;
            else if (value == "off") options.canonical_frontier_bootstrap = false;
            else throw std::invalid_argument("canonical frontier must be on or off");
        } else if (arg == "--max-proof-regions") {
            options.max_proof_regions = parse_size(value_after(arg), "maximum proof forest regions");
        } else if (arg == "--anneal-min-vertices") {
            options.annealing_min_vertices = parse_size(value_after(arg), "annealing minimum vertices");
        } else if (arg == "--descending-steps") {
            const auto value = parse_size(value_after(arg), "descending steps");
            if (value > std::numeric_limits<unsigned>::max())
                throw std::invalid_argument("descending steps is too large");
            options.descending_feasible_steps = static_cast<unsigned>(value);
        } else if (arg == "--sdp-iterations") {
            options.sdp_iterations = parse_size(value_after(arg), "SDP iterations");
        } else if (arg == "--sdp-max-dimension") {
            options.sdp_max_dimension = parse_size(value_after(arg), "SDP maximum dimension");
            if (options.sdp_max_dimension == 0)
                throw std::invalid_argument("SDP maximum dimension must be positive");
        } else if (arg == "--sdp-backend") {
            const auto value = value_after(arg);
            if (value == "admm") options.sdp_backend = cutwidth::SdpBackend::dense_admm;
            else if (value == "clarabel") options.sdp_backend = cutwidth::SdpBackend::clarabel;
            else if (value == "clarabel-bisection")
                options.sdp_backend = cutwidth::SdpBackend::clarabel_bisection;
            else throw std::invalid_argument("SDP backend must be admm, clarabel, or clarabel-bisection");
        } else if (arg == "--sdp-time") {
            options.sdp_time = parse_seconds(value_after(arg));
        } else if (arg == "--sdp-max-cone-entries") {
            options.sdp_max_cone_entries = parse_size(value_after(arg), "SDP cone entry cap");
            if (options.sdp_max_cone_entries == 0)
                throw std::invalid_argument("SDP cone entry cap must be positive");
        } else if (arg == "--sdp-bisection-offsets") {
            options.sdp_bisection_offsets = parse_size_list(
                value_after(arg), "SDP bisection offsets");
        } else if (arg == "--sdp-triangle-cuts") {
            options.sdp_triangle_cuts = parse_size(value_after(arg), "SDP triangle cut limit");
        } else if (arg == "--sdp-quantization-bits") {
            const auto bits = parse_size(value_after(arg), "SDP quantization bits");
            if (bits < 2 || bits > 50)
                throw std::invalid_argument("SDP quantization bits must be between 2 and 50");
            options.sdp_quantization_bits = static_cast<unsigned>(bits);
        } else if (arg == "--sdp-schedule") {
            const auto value = value_after(arg);
            if (value == "off") options.sdp_schedule = cutwidth::sdp::SdpSchedule::off;
            else if (value == "root") options.sdp_schedule = cutwidth::sdp::SdpSchedule::root;
            else if (value == "adaptive")
                options.sdp_schedule = cutwidth::sdp::SdpSchedule::adaptive;
            else throw std::invalid_argument("SDP schedule must be off, root, or adaptive");
        } else if (arg == "--sdp-total-time") {
            options.sdp_total_time = parse_seconds(value_after(arg));
        } else if (arg == "--sdp-max-calls") {
            options.sdp_max_calls = parse_size(value_after(arg), "SDP maximum calls");
        } else if (arg == "--sdp-max-state-dimension") {
            options.sdp_max_state_dimension = parse_size(
                value_after(arg), "SDP maximum state dimension");
        } else if (arg == "--sdp-trigger-nodes") {
            options.sdp_trigger_nodes = parse_size(value_after(arg), "SDP trigger nodes");
        } else if (arg == "--features") {
            options.features = value_after(arg);
        } else if (arg == "-i" || arg == "--input") {
            if (options.input) {
                throw std::invalid_argument("input specified more than once");
            }
            options.input = value_after(arg);
        } else if (arg == "-t" || arg == "--time-limit") {
            options.time_limit = parse_seconds(value_after(arg));
        } else if (arg == "-k" || arg == "--max-width") {
            if (options.max_width) throw std::invalid_argument("maximum width specified more than once");
            options.max_width = parse_width(value_after(arg));
        } else if (arg == "-") {
            if (options.input) {
                throw std::invalid_argument("input specified more than once");
            }
            options.input = arg;
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::invalid_argument("unknown option: " + arg);
        } else {
            if (options.input) {
                throw std::invalid_argument("input specified more than once");
            }
            options.input = arg;
        }
    }
    return options;
}

template <typename Options>
void apply_features(const CliOptions& cli, Options& options) {
    options.backend = cli.backend;
    options.failed_state_cache_memory_bytes = cli.cache_memory_bytes;
    options.cache_replacement = cli.cache_replacement;
    options.cache_replacement_page_capacity =
        cli.cache_replacement_page_capacity;
    options.threads = cli.threads;
    options.depth_two_lookahead_max_remaining = cli.lookahead_max_remaining;
    options.node_state = cli.node_state;
    options.node_order = cli.node_order;
    options.node_memo_depth = cli.node_memo_depth;
    options.node_memo_max_remaining = cli.node_memo_max_remaining;
    options.node_memo_memory_bytes = cli.node_memo_memory_bytes;
    options.parallel_min_cache_shards = cli.parallel_min_cache_shards;
    options.parallel_cache_shards_per_thread = cli.parallel_cache_shards_per_thread;
    options.parallel_runtime = cli.parallel_runtime;
    options.use_canonical_ownership = cli.use_canonical_ownership;
    options.cooperative_work_stealing = cli.cooperative_work_stealing;
    options.canonical_frontier_bootstrap = cli.canonical_frontier_bootstrap;
    options.recursive_coarse_kernel = cli.recursive_coarse_kernel;
    options.controller_overhead_fraction = cli.controller_overhead_fraction;
    options.max_proof_regions = cli.max_proof_regions;
    options.best_next_buckets = cli.best_next_buckets;
    options.candidate_enumerator = cli.candidate_enumerator;
    options.local_continuation_depth = cli.local_continuation_depth;
    options.local_continuation_max_slack = cli.local_continuation_max_slack;
    options.local_continuation_max_children = cli.local_continuation_max_children;
    options.local_continuation_max_states = cli.local_continuation_max_states;
    options.partial_bounds.expensive_max_slack = cli.partial_bound_max_slack;
    options.partial_bounds.lagrangian_prefix_bound = cli.lagrangian_prefix_bound;
    options.partial_bounds.lagrangian_max_slack = cli.lagrangian_max_slack;
    options.partial_bounds.lagrangian_max_residual = cli.lagrangian_max_residual;
    options.partial_bounds.lagrangian_denominator = cli.lagrangian_denominator;
    if (cli.lagrangian_prefix_bound) {
        options.use_partial_bounds = true;
    }
    if (!cli.features) return;
    options.use_failed_state_cache = false;
    options.use_twin_symmetry = false;
    options.use_depth_two_lookahead = false;
    options.use_partial_bounds = false;
    options.partial_bounds = {};
    options.partial_bounds.expensive_max_slack = cli.partial_bound_max_slack;
    options.partial_bounds.lagrangian_prefix_bound = cli.lagrangian_prefix_bound;
    options.partial_bounds.lagrangian_max_slack = cli.lagrangian_max_slack;
    options.partial_bounds.lagrangian_max_residual = cli.lagrangian_max_residual;
    options.partial_bounds.lagrangian_denominator = cli.lagrangian_denominator;
    if (cli.lagrangian_prefix_bound) {
        options.use_partial_bounds = true;
    }
    options.partial_bounds.residual_degree = false;
    options.partial_bounds.edge_distance_area = false;
    options.partial_bounds.degree_distance_area = false;
    options.partial_bounds.degeneracy = false;
    if (*cli.features == "none") { options.use_failed_state_cache = false; return; }
    if (*cli.features == "all") {
        options.use_failed_state_cache = true;
        options.use_twin_symmetry = true;
        options.use_depth_two_lookahead = true;
        options.use_partial_bounds = true;
        options.partial_bounds = {true, true, true, true};
        options.partial_bounds.lagrangian_prefix_bound = cli.lagrangian_prefix_bound;
        options.partial_bounds.lagrangian_max_slack = cli.lagrangian_max_slack;
        options.partial_bounds.lagrangian_max_residual = cli.lagrangian_max_residual;
        options.partial_bounds.lagrangian_denominator = cli.lagrangian_denominator;
        if (cli.lagrangian_prefix_bound) {
            options.use_partial_bounds = true;
        }
        return;
    }
    std::istringstream input(*cli.features);
    std::string feature;
    while (std::getline(input, feature, ',')) {
        if (feature == "cache") options.use_failed_state_cache = true;
        else if (feature == "twins") options.use_twin_symmetry = true;
        else if (feature == "lookahead") options.use_depth_two_lookahead = true;
        else if (feature == "residual-degree") {
            options.use_partial_bounds = true; options.partial_bounds.residual_degree = true;
        } else if (feature == "area-edge") {
            options.use_partial_bounds = true; options.partial_bounds.edge_distance_area = true;
        } else if (feature == "area-degree") {
            options.use_partial_bounds = true; options.partial_bounds.degree_distance_area = true;
        } else if (feature == "degeneracy") {
            options.use_partial_bounds = true; options.partial_bounds.degeneracy = true;
        } else throw std::invalid_argument("unknown feature: " + feature);
    }
}

template <typename Range, typename PrintValue>
void print_json_array(std::ostream& out, const Range& values, PrintValue print_value) {
    out << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!first) out << ',';
        first = false;
        print_value(value);
    }
    out << ']';
}

void print_json_string(std::ostream& out, std::string_view value) {
    out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20 || c >= 0x80) {
                out << "\\u00" << hex[c >> 4] << hex[c & 0x0f];
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    out << '"';
}

template <std::size_t N>
void print_u64_array(std::ostream& out,
                     const std::array<std::uint64_t, N>& values) {
    print_json_array(out, values, [&](std::uint64_t value) { out << value; });
}

void write_dfs_diagnostics(const std::filesystem::path& path,
                           const cutwidth::Graph& graph,
                           const cutwidth::DecisionStats& stats) {
    const auto vertex_count = graph.size();
    const auto& diagnostics = stats.dfs_diagnostics;
    if (!diagnostics.enabled)
        throw std::runtime_error("requested DFS diagnostics were not collected");
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open DFS diagnostics file: " + temporary.string());
    auto print_bound = [&](const cutwidth::DfsBoundDiagnostics& bound) {
        out << "{\"evaluations\":" << bound.evaluations
            << ",\"prunes\":" << bound.prunes
            << ",\"nanoseconds\":" << bound.nanoseconds << '}';
    };
    out << "{\"schema_version\":1,\"threshold\":" << diagnostics.threshold
        << ",\"vertices\":" << vertex_count
        << ",\"nodes_entered\":" << diagnostics.nodes_entered
        << ",\"viable_child_buckets\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5-7\",\"8-15\",\"16-31\",\"32-63\",\"64+\"]"
        << ",\"slack_buckets\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5-7\",\"8-15\",\"16+\"]"
        << ",\"cache\":{\"queries\":" << stats.failed_cache_queries
        << ",\"hits\":" << stats.failed_cache_hits
        << ",\"states_recorded\":" << stats.failed_states_recorded
        << ",\"collisions\":" << stats.cache_collisions
        << ",\"lookup_probes\":" << stats.cache_lookup_probes
        << ",\"insertion_probes\":" << stats.cache_insertion_probes
        << ",\"insertions_rejected_at_capacity\":"
        << stats.failed_state_insertions_skipped
        << ",\"eviction_policy\":\"none-saturating\"}"
        << ",\"bounds\":{\"sdp\":";
    print_bound(diagnostics.sdp_bound);
    out << ",\"partial\":";
    print_bound(diagnostics.partial_bound);
    out << ",\"residual_dp\":";
    print_bound(diagnostics.residual_dp);
    out << ",\"best_next\":";
    print_bound(diagnostics.best_next_bound);
    out << ",\"partial_prunes_by_kind\":{"
        << "\"residual_degree\":" << stats.partial_bounds.residual_degree_prunes
        << ",\"edge_distance_area\":" << stats.partial_bounds.edge_distance_area_prunes
        << ",\"degree_distance_area\":" << stats.partial_bounds.degree_distance_area_prunes
        << ",\"degeneracy\":" << stats.partial_bounds.degeneracy_prunes
        << ",\"lagrangian\":" << stats.partial_bounds.lagrangian_certified_prunes
        << "},\"lagrangian_stats\":{"
        << "\"evaluations\":" << stats.partial_bounds.lagrangian_evaluations
        << ",\"mincuts\":" << stats.partial_bounds.lagrangian_mincuts
        << ",\"slack_gate_skips\":" << stats.partial_bounds.lagrangian_slack_gate_skips
        << ",\"residual_gate_skips\":" << stats.partial_bounds.lagrangian_residual_gate_skips
        << ",\"ineligible_gate_skips\":" << stats.partial_bounds.lagrangian_ineligible_gate_skips
        << ",\"overflow_gate_skips\":" << stats.partial_bounds.lagrangian_overflow_gate_skips
        << "}}";
    out << ",\"depths\":[";
    bool first = true;
    for (std::size_t depth = 0; depth < diagnostics.by_depth.size(); ++depth) {
        const auto& entry = diagnostics.by_depth[depth];
        if (entry.nodes_entered == 0 && entry.viable_child_observations == 0) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"depth\":" << depth
            << ",\"nodes_entered\":" << entry.nodes_entered
            << ",\"cache_queries\":" << entry.cache_queries
            << ",\"cache_hits\":" << entry.cache_hits
            << ",\"prunes\":{\"cache\":" << entry.cache_prunes
            << ",\"sdp\":" << entry.sdp_prunes
            << ",\"partial_bound\":" << entry.partial_bound_prunes
            << ",\"residual_dp\":" << entry.residual_dp_prunes
            << ",\"best_next\":" << entry.best_next_prunes
            << ",\"dead_end\":" << entry.dead_ends << "}"
            << ",\"child_rejections\":{\"cut\":" << entry.children_rejected_by_cut
            << ",\"symmetry\":" << entry.children_rejected_by_symmetry
            << ",\"lookahead\":" << entry.children_rejected_by_lookahead << "}"
            << ",\"viable_child_observations\":" << entry.viable_child_observations
            << ",\"viable_children_sum\":" << entry.viable_children_sum
            << ",\"viable_child_histogram\":";
        print_u64_array(out, entry.viable_child_histogram);
        out << ",\"slack_histogram\":";
        print_u64_array(out, entry.slack_histogram);
        out << '}';
    }
    out << "],\"roots\":[";
    auto roots = diagnostics.by_root;
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
        return a.root_vertex < b.root_vertex;
    });
    first = true;
    for (const auto& entry : roots) {
        if (entry.nodes_entered == 0) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"root_vertex\":";
        if (entry.root_vertex == vertex_count) out << "null";
        else out << entry.root_vertex;
        out << ",\"root_label\":";
        if (entry.root_vertex == vertex_count) out << "null";
        else print_json_string(out, graph.label(entry.root_vertex));
        out << ",\"nodes_entered\":" << entry.nodes_entered
            << ",\"total_prunes\":" << entry.total_prunes
            << ",\"cache_prunes\":" << entry.cache_prunes
            << ",\"bound_prunes\":" << entry.bound_prunes
            << ",\"dead_ends\":" << entry.dead_ends
            << ",\"maximum_depth\":" << entry.maximum_depth
            << ",\"nodes_by_depth\":";
        print_json_array(out, entry.nodes_by_depth,
            [&](std::uint64_t value) { out << value; });
        out << '}';
    }
    out << "]}\n";
    out.flush();
    if (!out) throw std::runtime_error("failed writing DFS diagnostics: " + temporary.string());
    out.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) throw std::system_error(error, "cannot replace DFS diagnostics file");
}

std::string_view milp_status_name(cutwidth::MilpStatus status) {
    switch (status) {
    case cutwidth::MilpStatus::optimal: return "OPTIMAL";
    case cutwidth::MilpStatus::infeasible: return "INFEASIBLE";
    case cutwidth::MilpStatus::limit: return "LIMIT";
    case cutwidth::MilpStatus::unavailable: return "UNAVAILABLE";
    case cutwidth::MilpStatus::error: return "ERROR";
    case cutwidth::MilpStatus::unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string_view candidate_enumerator_name(cutwidth::CandidateEnumerator mode) {
    switch (mode) {
    case cutwidth::CandidateEnumerator::scan: return "scan";
    case cutwidth::CandidateEnumerator::delta_buckets: return "delta-buckets";
    case cutwidth::CandidateEnumerator::cross_check: return "cross-check";
    }
    return "unknown";
}

std::string_view clarabel_sdp_status_name(int status) {
    switch (status) {
    case 0: return "SOLVED";
    case 1: return "ALMOST_SOLVED";
    case 2: return "MAX_ITERATIONS";
    case 3: return "MAX_TIME";
    case 4: return "INFEASIBLE";
    case 5: return "NUMERICAL_ERROR";
    case 6: return "UNAVAILABLE";
    case 7: return "UNSUPPORTED";
    default: return "UNKNOWN";
    }
}

cutwidth::pb::DecisionOptions make_pb_options(
    const CliOptions& cli, std::chrono::milliseconds time_limit) {
    cutwidth::pb::DecisionOptions options;
    options.encoding = cli.pb_encoding;
    options.solver = cli.pb_solver;
    options.channel_positions = cli.pb_channel_positions;
    options.split_first_vertex = cli.pb_split_first;
    options.native_incremental = cli.pb_native_incremental;
    options.workers = cli.threads;
    options.external.solver_path = cli.pb_solver_path;
    options.external.solver_arguments = {"{input}", "{proof}"};
    options.external.proof_checker_path = cli.pb_proof_checker;
    options.external.proof_checker_arguments = {"{input}", "{proof}"};
    options.external.expected_version = cli.pb_expected_version;
    options.external.time_limit = time_limit;
    options.external.proof_check_reserve = cli.pb_proof_check_time;
    options.external.keep_temporary_files = cli.pb_keep_files;
    return options;
}

int run(const CliOptions& cli) {
    if (cli.cache_memory_bytes != 0 &&
        cli.node_memo_memory_bytes > cli.cache_memory_bytes)
        throw std::invalid_argument("node memo memory exceeds cache memory");
    if (cli.pb_native_incremental && cli.pb_split_first)
        throw std::invalid_argument(
            "native incremental PB and external first-vertex splitting are separate modes");
    if (cli.controller == cutwidth::ControllerMode::adaptive &&
        cli.cache_mode == cutwidth::CacheMode::cross_threshold)
        throw std::invalid_argument(
            "adaptive controller requires auto or fixed-threshold cache mode");
    if (cli.cache_replacement ==
            cutwidth::CacheReplacementPolicy::generational_clock &&
        cli.cache_mode == cutwidth::CacheMode::cross_threshold)
        throw std::invalid_argument(
            "generational cache replacement requires fixed-threshold cache mode");
    if (cli.cache_replacement ==
            cutwidth::CacheReplacementPolicy::generational_clock &&
        (cli.cache_replacement_page_capacity < 16 ||
         !std::has_single_bit(cli.cache_replacement_page_capacity)))
        throw std::invalid_argument(
            "cache replacement page capacity must be a power of two at least 16");
    if (cli.persistent_decision_session && !cli.max_width &&
        cli.controller != cutwidth::ControllerMode::adaptive)
        throw std::invalid_argument(
            "persistent decision session requires --max-width or --controller adaptive");
    if (cli.persistent_decision_session && cli.proof_backend != cutwidth::ProofBackend::dfs)
        throw std::invalid_argument("persistent decision session requires the DFS backend");
    if (cli.persistent_decision_session && cli.milp_time.count() != 0)
        throw std::invalid_argument("persistent decision session cannot be combined with MILP");
    if (cli.recursive_coarse_kernel && !cli.persistent_decision_session)
        throw std::invalid_argument(
            "coarse-recursive kernel requires --decision-session persistent");
    if (cli.recursive_coarse_kernel && !cli.canonical_frontier_bootstrap)
        throw std::invalid_argument(
            "coarse-recursive kernel requires --canonical-frontier on");
    if (cli.dfs_diagnostics && !cli.max_width)
        throw std::invalid_argument("DFS diagnostics requires --max-width");
    if (cli.dfs_diagnostics && cli.proof_backend != cutwidth::ProofBackend::dfs)
        throw std::invalid_argument("DFS diagnostics requires the DFS backend");
    if (cli.dfs_diagnostics && cli.milp_time.count() != 0)
        throw std::invalid_argument("DFS diagnostics cannot be combined with MILP");
    std::ifstream file;
    std::istream* input = &std::cin;
    if (cli.input && *cli.input != "-") {
        file.open(*cli.input);
        if (!file) throw std::runtime_error("cannot open input file: " + *cli.input);
        input = &file;
    }

    const auto graph = cutwidth::Graph::read_edge_list(*input);
    if (cli.cache_replacement ==
            cutwidth::CacheReplacementPolicy::generational_clock &&
        (cli.backend == cutwidth::DecisionBackend::word64 ||
         (cli.backend == cutwidth::DecisionBackend::automatic &&
          graph.supports_mask())))
        throw std::invalid_argument(
            "generational cache replacement requires the dynamic decision backend");
    if (cli.dfs_diagnostics &&
        (cli.backend == cutwidth::DecisionBackend::word64 ||
         (cli.backend == cutwidth::DecisionBackend::automatic && graph.supports_mask())))
        throw std::invalid_argument(
            "DFS diagnostics currently requires the dynamic decision backend");
    if (cli.candidate_enumerator != cutwidth::CandidateEnumerator::scan &&
        (cli.backend == cutwidth::DecisionBackend::word64 ||
         (cli.backend == cutwidth::DecisionBackend::automatic && graph.supports_mask())))
        throw std::invalid_argument(
            "delta-bucket candidate enumeration requires the dynamic decision backend");
    if (cli.local_continuation_depth != 0 &&
        (cli.backend == cutwidth::DecisionBackend::word64 ||
         (cli.backend == cutwidth::DecisionBackend::automatic && graph.supports_mask())))
        throw std::invalid_argument(
            "local continuation probing requires the dynamic decision backend");
    if (cli.local_continuation_depth != 0 &&
        cli.candidate_enumerator == cutwidth::CandidateEnumerator::scan)
        throw std::invalid_argument(
            "local continuation probing requires delta-bucket candidate enumeration");
    if (!graph.supports_mask() &&
        (cli.engine == "legacy" || cli.backend == cutwidth::DecisionBackend::word64)) {
        if (cli.json) {
            std::cout << "{\"schema_version\":3,\"engine\":\"" << cli.engine
                      << "\",\"status\":\"UNSUPPORTED\",\"vertices\":" << graph.size()
                      << ",\"edges\":" << graph.edge_count() << ",\"ordering\":[]";
            if (cli.max_width) std::cout << ",\"threshold\":" << *cli.max_width;
            std::cout << ",\"verified\":false}\n";
        } else {
            std::cout << "status: UNSUPPORTED\nengine: " << cli.engine
                      << "\nreason: selected backend supports at most 63 vertices\n";
        }
        return 0;
    }

    if (cli.max_width) {
        if (cli.engine == "legacy") throw std::invalid_argument("legacy engine cannot be combined with --max-width");
        cutwidth::DecisionOptions options;
        options.time_limit = cli.time_limit;
        options.cache_mode = cli.cache_mode == cutwidth::CacheMode::cross_threshold
            ? cutwidth::CacheMode::cross_threshold : cutwidth::CacheMode::fixed_threshold;
        apply_features(cli, options);
        options.residual_dp_max_bytes = cli.residual_dp_max_bytes;
        options.dfs_residual_dp_max_remaining = cli.dfs_residual_dp_max_remaining;
        options.collect_dfs_diagnostics = cli.dfs_diagnostics.has_value();
        const auto started = std::chrono::steady_clock::now();
        cutwidth::DecisionResult decision;
        bool milp_conclusive = false;
        cutwidth::MilpStatus milp_status = cutwidth::MilpStatus::unknown;
        double milp_build_seconds = 0.0, milp_solve_seconds = 0.0;
        double worker_busy_seconds = 0.0, worker_allocated_seconds = 0.0;
        std::int64_t milp_nodes = 0;
        std::optional<double> milp_dual_bound;
        cutwidth::pb::DecisionProvenance pb_provenance;
        bool pb_attempted = false;
        if (cli.milp_time.count() > 0) {
            double budget = static_cast<double>(cli.milp_time.count()) / 1000.0;
            if (cli.time_limit.count() > 0)
                budget = std::min(budget, static_cast<double>(cli.time_limit.count()) / 1000.0);
            const auto milp = cutwidth::run_highs(graph, {{*cli.max_width}}, budget);
            milp_status = milp.status;
            milp_build_seconds = milp.model_build_seconds;
            milp_solve_seconds = milp.solve_seconds;
            milp_nodes = milp.mip_nodes;
            milp_dual_bound = milp.diagnostic_dual_bound;
            if (milp.status == cutwidth::MilpStatus::optimal && milp.incumbent_width &&
                *milp.incumbent_width <= *cli.max_width && graph.validate_ordering(milp.ordering)) {
                decision.status = cutwidth::DecisionStatus::feasible;
                decision.threshold = *cli.max_width;
                decision.ordering = milp.ordering;
                milp_conclusive = true;
            } else if (milp.status == cutwidth::MilpStatus::infeasible) {
                decision.status = cutwidth::DecisionStatus::infeasible;
                decision.threshold = *cli.max_width;
                milp_conclusive = true;
            } else if (milp.status == cutwidth::MilpStatus::limit && milp.incumbent_width &&
                       *milp.incumbent_width <= *cli.max_width &&
                       graph.validate_ordering(milp.ordering)) {
                decision.status = cutwidth::DecisionStatus::feasible;
                decision.threshold = *cli.max_width;
                decision.ordering = milp.ordering;
                milp_conclusive = true;
            }
        }
        if (!milp_conclusive) {
            if (cli.time_limit.count() > 0) {
                const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                options.time_limit = used >= cli.time_limit
                    ? std::chrono::milliseconds{1} : cli.time_limit - used;
            }
            if (cli.proof_backend == cutwidth::ProofBackend::pb) {
                auto pb_result = cutwidth::pb::decide_cutwidth(
                    graph, *cli.max_width, make_pb_options(cli, options.time_limit));
                decision = std::move(pb_result.decision);
                pb_provenance = std::move(pb_result.provenance);
                pb_attempted = true;
            } else if (cli.persistent_decision_session) {
                options.memory_governor =
                    std::make_shared<cutwidth::MemoryGovernor>(cli.memory_budget_bytes);
                const auto deadline = options.time_limit.count() == 0
                    ? std::chrono::steady_clock::time_point::max()
                    : std::chrono::steady_clock::now() + options.time_limit;
                cutwidth::ParallelDecisionSession session(
                    graph, *cli.max_width, options, cli.threads);
                const auto event = session.service({
                    std::numeric_limits<std::uint64_t>::max(), deadline});
                decision.threshold = *cli.max_width;
                decision.stats = event.delta;
                decision.stats.parallel_workers_used = event.workers_used;
                decision.stats.parallel_root_tasks_started = event.donations;
                decision.stats.parallel_root_tasks_completed = event.terminal_regions;
                decision.stats.configured_proof_regions_bound = event.configured_proof_regions_bound;
                decision.stats.resolved_proof_regions_bound = event.resolved_proof_regions_bound;
                decision.stats.peak_proof_regions = event.peak_proof_regions;
                decision.stats.suppressed_donations = event.suppressed_donations;
                worker_busy_seconds = event.busy_worker_seconds;
                worker_allocated_seconds = event.allocated_worker_seconds;
                if (event.status == cutwidth::SessionStatus::feasible) {
                    decision.status = cutwidth::DecisionStatus::feasible;
                    decision.ordering = session.ordering();
                } else if (event.status == cutwidth::SessionStatus::infeasible) {
                    decision.status = cutwidth::DecisionStatus::infeasible;
                } else {
                    decision.status = cutwidth::DecisionStatus::timed_out;
                }
            } else {
                decision = cutwidth::decide_cutwidth_v2(graph, *cli.max_width, options);
            }
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const char* status = decision.status == cutwidth::DecisionStatus::feasible ? "FEASIBLE" :
                             decision.status == cutwidth::DecisionStatus::infeasible ? "INFEASIBLE" : "UNKNOWN";
        bool verified = false;
        if (cli.verify && decision.status == cutwidth::DecisionStatus::feasible) {
            if (!graph.validate_ordering(decision.ordering) ||
                graph.ordering_cutwidth(decision.ordering) > *cli.max_width)
                throw std::runtime_error("verification failed: decision witness exceeds threshold");
            verified = true;
        }
        if (cli.dfs_diagnostics)
            write_dfs_diagnostics(*cli.dfs_diagnostics, graph, decision.stats);
        if (cli.json) {
            std::cout << "{\"schema_version\":3,\"engine\":\"" << cli.engine
                      << "\",\"mode\":\"decision\",\"status\":\"" << status << "\","
                      << "\"decision_session\":\""
                      << (cli.persistent_decision_session ? "persistent" : "classic")
                      << "\",\"persistent_kernel\":\""
                      << (cli.recursive_coarse_kernel ? "coarse-recursive" : "stack")
                      << "\","
                      << "\"threshold\":" << *cli.max_width << ','
                      << "\"vertices\":" << graph.size() << ",\"edges\":" << graph.edge_count()
                      << ",\"ordering\":";
            print_json_array(std::cout, decision.ordering,
                [&](std::size_t vertex) { print_json_string(std::cout, graph.label(vertex)); });
            std::cout << ",\"runtime_seconds\":" << std::fixed << std::setprecision(6) << elapsed
                      << ",\"proof_backend\":\""
                      << (cli.proof_backend == cutwidth::ProofBackend::pb ? "pb" : "dfs") << '"'
                      << ",\"nodes_expanded\":" << decision.stats.nodes_expanded
                      << ",\"dfs_min_remaining_vertices\":" << (decision.stats.dfs_min_remaining_vertices == std::numeric_limits<std::uint32_t>::max() ? "null" : std::to_string(decision.stats.dfs_min_remaining_vertices))
                      << ",\"parallel_workers_used\":"
                      << decision.stats.parallel_workers_used
                      << ",\"parallel_root_tasks_started\":"
                      << decision.stats.parallel_root_tasks_started
                      << ",\"parallel_root_tasks_completed\":"
                      << decision.stats.parallel_root_tasks_completed
                      << ",\"worker_busy_seconds\":";
            if (cli.persistent_decision_session) std::cout << worker_busy_seconds;
            else std::cout << "null";
            std::cout << ",\"worker_allocated_seconds\":";
            if (cli.persistent_decision_session) std::cout << worker_allocated_seconds;
            else std::cout << "null";
            std::cout
                      << ",\"children_rejected_by_cut\":" << decision.stats.children_rejected_by_cut
                      << ",\"failed_cache_hits\":" << decision.stats.failed_cache_hits
                      << ",\"best_next_bucket_checks\":" << decision.stats.best_next_bucket_checks
                      << ",\"best_next_bucket_parent_prunes\":" << decision.stats.best_next_bucket_parent_prunes
                      << ",\"best_next_bucket_candidates_avoided\":" << decision.stats.best_next_bucket_candidates_avoided
                      << ",\"candidate_enumerator\":\""
                      << candidate_enumerator_name(cli.candidate_enumerator) << '"'
                      << ",\"candidate_scan_checks\":" << decision.stats.candidate_scan_checks
                      << ",\"candidate_index_gathers\":" << decision.stats.candidate_index_gathers
                      << ",\"candidate_index_bucket_slots_visited\":"
                      << decision.stats.candidate_index_bucket_slots_visited
                      << ",\"candidate_index_vertices_emitted\":"
                      << decision.stats.candidate_index_vertices_emitted
                      << ",\"candidate_index_forward_updates\":"
                      << decision.stats.candidate_index_forward_updates
                      << ",\"candidate_index_rollback_updates\":"
                      << decision.stats.candidate_index_rollback_updates
                      << ",\"candidate_index_cross_checks\":"
                      << decision.stats.candidate_index_cross_checks
                      << ",\"local_continuation_calls\":"
                      << decision.stats.local_continuation_calls
                      << ",\"local_continuation_slack_gate_skips\":"
                      << decision.stats.local_continuation_slack_gate_skips
                      << ",\"local_continuation_branch_gate_skips\":"
                      << decision.stats.local_continuation_branch_gate_skips
                      << ",\"local_continuation_inconclusive\":"
                      << decision.stats.local_continuation_inconclusive
                      << ",\"local_continuation_states\":"
                      << decision.stats.local_continuation_states
                      << ",\"local_continuation_parent_prunes\":"
                      << decision.stats.local_continuation_parent_prunes
                      << ",\"local_continuation_nanoseconds\":"
                      << decision.stats.local_continuation_nanoseconds
                      << ",\"local_continuation_cross_checks\":"
                      << decision.stats.local_continuation_cross_checks
                      << ",\"milp_attempted\":" << (cli.milp_time.count() > 0 ? "true" : "false")
                      << ",\"milp_status\":\"" << milp_status_name(milp_status) << '"'
                      << ",\"milp_conclusive\":" << (milp_conclusive ? "true" : "false")
                      << ",\"milp_model_build_seconds\":" << milp_build_seconds
                      << ",\"milp_solve_seconds\":" << milp_solve_seconds
                      << ",\"milp_nodes\":" << milp_nodes
                      << ",\"milp_diagnostic_dual_bound\":";
            if (milp_dual_bound) std::cout << *milp_dual_bound;
            else std::cout << "null";
            std::cout
                      << ",\"pb_attempted\":" << (pb_attempted ? "true" : "false")
                      << ",\"pb_native_incremental\":"
                      << (cli.pb_native_incremental ? "true" : "false")
                      << ",\"pb_solver\":\"" << cutwidth::pb::solver_name(cli.pb_solver) << '"'
                      << ",\"pb_encoding\":\"" << cutwidth::pb::encoding_name(cli.pb_encoding) << '"'
                      << ",\"pb_position_channeling\":"
                      << (pb_provenance.position_channeling ? "true" : "false")
                      << ",\"pb_first_vertex_split\":"
                      << (pb_provenance.first_vertex_split ? "true" : "false")
                      << ",\"pb_branches_total\":" << pb_provenance.branches_total
                      << ",\"pb_branches_completed\":" << pb_provenance.branches_completed
                      << ",\"pb_branches_unsat_verified\":"
                      << pb_provenance.branches_unsat_verified
                      << ",\"pb_branches_sat_verified\":"
                      << pb_provenance.branches_sat_verified
                      << ",\"pb_branches_unsat_unverified\":"
                      << pb_provenance.branches_unsat_unverified
                      << ",\"pb_branches_timed_out\":"
                      << pb_provenance.branches_timed_out
                      << ",\"pb_branches_other_failures\":"
                      << pb_provenance.branches_other_failures
                      << ",\"pb_branches_with_proof\":"
                      << pb_provenance.branches_with_proof
                      << ",\"pb_status\":\""
                      << cutwidth::pb::status_name(pb_provenance.external_status) << '"'
                      << ",\"pb_model_hash\":";
            if (pb_attempted) print_json_string(std::cout, pb_provenance.model_hash);
            else std::cout << "null";
            std::cout << ",\"pb_variables\":" << pb_provenance.variables
                      << ",\"pb_clauses\":" << pb_provenance.clauses
                      << ",\"pb_proof_generated\":"
                      << (pb_provenance.proof_generated ? "true" : "false")
                      << ",\"pb_proof_checked\":"
                      << (pb_provenance.proof_checked ? "true" : "false")
                      << ",\"pb_proof_bytes\":" << pb_provenance.proof_bytes
                      << ",\"pb_proof_hash\":";
            if (!pb_provenance.proof_hash.empty())
                print_json_string(std::cout, pb_provenance.proof_hash);
            else std::cout << "null";
            std::cout << ",\"pb_artifact_directory\":";
            if (!pb_provenance.artifact_directory.empty())
                print_json_string(std::cout, pb_provenance.artifact_directory);
            else std::cout << "null";
            std::cout
                      << ",\"pb_witness_verified\":"
                      << (pb_provenance.witness_verified ? "true" : "false")
                      << ",\"pb_solver_version\":";
            if (pb_attempted) print_json_string(std::cout, pb_provenance.solver_version);
            else std::cout << "null";
            std::cout
                      << ",\"failed_cache_queries\":" << decision.stats.failed_cache_queries
                      << ",\"failed_states_recorded\":" << decision.stats.failed_states_recorded
                      << ",\"cache_memory_bytes\":" << decision.stats.failed_state_cache_memory_bytes
                      << ",\"cache_collisions\":" << decision.stats.cache_collisions
                      << ",\"cache_segment_growths\":"
                      << decision.stats.cache_segment_growths
                      << ",\"cache_lookup_probes\":"
                      << decision.stats.cache_lookup_probes
                      << ",\"cache_insertion_probes\":"
                      << decision.stats.cache_insertion_probes
                      << ",\"cache_probes_avoided_after_saturation\":"
                      << decision.stats.cache_probes_avoided_after_saturation
                      << ",\"cache_page_promotions\":" << decision.stats.cache_page_promotions
                      << ",\"cache_page_second_chances\":" << decision.stats.cache_page_second_chances
                      << ",\"cache_pages_recycled\":" << decision.stats.cache_pages_recycled
                      << ",\"cache_replacement_admissions\":" << decision.stats.cache_replacement_admissions
                      << ",\"cache_entries_evicted\":" << decision.stats.cache_entries_evicted
                      << ",\"cache_evicted_depth_sum\":" << decision.stats.cache_evicted_depth_sum
                      << ",\"cache_maximum_evicted_depth\":" << decision.stats.cache_maximum_evicted_depth
                      << ",\"unique_canonical_claims\":"
                      << decision.stats.unique_canonical_claims
                      << ",\"duplicate_ownership_waits\":"
                      << decision.stats.duplicate_ownership_waits
                      << ",\"ownership_saturation\":"
                      << decision.stats.ownership_saturation
                      << ",\"node_memo_available\":" << (decision.stats.node_memo_available ? "true" : "false")
                      << ",\"node_memo_hits_by_depth\":[";
            for (std::size_t d = 0; d < decision.stats.node_memo_hits_by_depth.size(); ++d) {
                if (d) std::cout << ',';
                std::cout << decision.stats.node_memo_hits_by_depth[d];
            }
            std::cout << "]"
                      << ",\"node_memo_computations\":" << decision.stats.node_memo_computations
                      << ",\"node_memo_prunes\":" << decision.stats.node_memo_prunes
                      << ",\"node_memo_child_rejections\":" << decision.stats.node_memo_child_rejections
                      << ",\"node_memo_collisions\":" << decision.stats.node_memo_collisions
                      << ",\"node_memo_saturation\":" << decision.stats.node_memo_saturation
                      << ",\"node_memo_memory_bytes\":" << decision.stats.node_memo_memory_bytes
                      << ",\"node_state_updates\":" << decision.stats.node_state_updates
                      << ",\"residual_histogram_updates\":"
                      << decision.stats.residual_histogram_updates
                      << ",\"node_sorts_avoided\":" << decision.stats.node_sorts_avoided
                      << ",\"partial_bound_evaluations\":" << decision.stats.partial_bounds.evaluations
                      << ",\"partial_bound_slack_gate_skips\":"
                      << decision.stats.partial_bounds.expensive_slack_gate_skips
                      << ",\"residual_degree_evaluations\":"
                      << decision.stats.partial_bounds.residual_degree_evaluations
                      << ",\"edge_area_evaluations\":"
                      << decision.stats.partial_bounds.edge_distance_area_evaluations
                      << ",\"degree_area_evaluations\":"
                      << decision.stats.partial_bounds.degree_distance_area_evaluations
                      << ",\"degeneracy_evaluations\":"
                      << decision.stats.partial_bounds.degeneracy_evaluations
                      << ",\"lagrangian_evaluations\":" << decision.stats.partial_bounds.lagrangian_evaluations
                      << ",\"lagrangian_mincuts\":" << decision.stats.partial_bounds.lagrangian_mincuts
                      << ",\"lagrangian_certified_prunes\":" << decision.stats.partial_bounds.lagrangian_certified_prunes
                      << ",\"lagrangian_slack_gate_skips\":" << decision.stats.partial_bounds.lagrangian_slack_gate_skips
                      << ",\"lagrangian_residual_gate_skips\":" << decision.stats.partial_bounds.lagrangian_residual_gate_skips
                      << ",\"lagrangian_ineligible_gate_skips\":" << decision.stats.partial_bounds.lagrangian_ineligible_gate_skips
                      << ",\"lagrangian_overflow_gate_skips\":" << decision.stats.partial_bounds.lagrangian_overflow_gate_skips
                      << ",\"residual_degree_prunes\":" << decision.stats.partial_bounds.residual_degree_prunes
                      << ",\"edge_area_prunes\":" << decision.stats.partial_bounds.edge_distance_area_prunes
                      << ",\"degree_area_prunes\":" << decision.stats.partial_bounds.degree_distance_area_prunes
                      << ",\"degeneracy_prunes\":" << decision.stats.partial_bounds.degeneracy_prunes
                      << ",\"twin_symmetric_children_skipped\":"
                      << decision.stats.twin_symmetric_children_skipped
                      << ",\"residual_dp_attempts\":" << decision.stats.residual_dp_attempts
                      << ",\"residual_dp_admissions\":" << decision.stats.residual_dp_admissions
                      << ",\"residual_dp_rejections\":" << decision.stats.residual_dp_governor_or_cap_rejections
                      << ",\"residual_dp_completed_tails\":" << decision.stats.residual_dp_completed_tails
                      << ",\"residual_dp_infeasible_prunes\":" << decision.stats.residual_dp_infeasible_prunes
                      << ",\"residual_dp_feasible_witnesses\":" << decision.stats.residual_dp_feasible_witnesses
                      << ",\"residual_dp_peak_bytes\":" << decision.stats.residual_dp_peak_bytes
                      << ",\"residual_dp_seconds\":" << std::fixed << std::setprecision(6) << decision.stats.residual_dp_seconds
                      << ",\"residual_dp_cold_restarts\":" << decision.stats.residual_dp_cold_restarts
                      << ",\"residual_dp_states\":" << decision.stats.residual_dp_states
                      << ",\"configured_proof_regions_bound\":" << decision.stats.configured_proof_regions_bound
                      << ",\"resolved_proof_regions_bound\":" << decision.stats.resolved_proof_regions_bound
                      << ",\"peak_proof_regions\":" << decision.stats.peak_proof_regions
                      << ",\"suppressed_donations\":" << decision.stats.suppressed_donations
                      << ",\"verified\":" << (verified ? "true" : "false") << "}\n";
        } else {
            std::cout << "mode: decision\nstatus: " << status
                      << "\nthreshold: " << *cli.max_width
                      << "\nvertices: " << graph.size() << "\nedges: " << graph.edge_count()
                      << "\nordering:";
            for (const auto vertex : decision.ordering) std::cout << ' ' << graph.label(vertex);
            std::cout << "\nruntime seconds: " << std::fixed << std::setprecision(6) << elapsed
                      << "\nnodes expanded: " << decision.stats.nodes_expanded
                      << "\ndfs min remaining vertices: ";
            if (decision.stats.dfs_min_remaining_vertices == std::numeric_limits<std::uint32_t>::max()) {
                std::cout << "null";
            } else {
                std::cout << decision.stats.dfs_min_remaining_vertices;
            }
            std::cout << "\nchildren rejected by cut: " << decision.stats.children_rejected_by_cut
                      << "\nfailed cache hits: " << decision.stats.failed_cache_hits
                      << "\nbest next bucket checks: " << decision.stats.best_next_bucket_checks
                      << "\nbest next bucket parent prunes: " << decision.stats.best_next_bucket_parent_prunes
                      << "\nbest next bucket candidates avoided: " << decision.stats.best_next_bucket_candidates_avoided
                      << "\nfailed states recorded: " << decision.stats.failed_states_recorded << '\n';
            std::cout << "twin-symmetric children skipped: "
                      << decision.stats.twin_symmetric_children_skipped << '\n';
            if (verified) std::cout << "verification: PASSED\n";
        }
        return 0;
    }

    cutwidth::SolverOptions solver_options;
    solver_options.time_limit = cli.time_limit;

    const auto started = std::chrono::steady_clock::now();
    cutwidth::SolverResult result;
    const std::string_view engine = cli.engine;
    std::uint64_t v2_decision_calls = 0;
    std::size_t parallel_workers_used = 1;
    std::uint64_t parallel_root_tasks_started = 0;
    std::uint64_t parallel_root_tasks_completed = 0;
    std::uint64_t v2_twin_skips = 0;
    std::uint64_t best_next_bucket_checks = 0;
    std::uint64_t best_next_bucket_parent_prunes = 0;
    std::uint64_t best_next_bucket_candidates_avoided = 0;
    std::uint64_t candidate_scan_checks = 0, candidate_index_gathers = 0;
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
    std::uint64_t lookahead_checks = 0, lookahead_rejections = 0;
    std::uint64_t cache_strengthenings = 0, cache_collisions = 0;
    std::uint64_t cache_segment_growths = 0, cache_lookup_probes = 0;
    std::uint64_t cache_insertion_probes = 0;
    std::uint64_t cache_probes_avoided_after_saturation = 0;
    std::uint64_t cache_page_promotions = 0, cache_page_second_chances = 0;
    std::uint64_t cache_pages_recycled = 0, cache_replacement_admissions = 0;
    std::uint64_t cache_entries_evicted = 0, cache_evicted_depth_sum = 0;
    std::uint32_t cache_maximum_evicted_depth = 0;
    std::uint64_t unique_canonical_claims = 0;
    std::uint64_t duplicate_ownership_waits = 0, ownership_saturation = 0;
    bool resumed_from_checkpoint = false;
    std::uint64_t checkpoint_elapsed_milliseconds = 0, checkpoints_written = 0;
    double checkpoint_write_seconds = 0.0;
    std::uint64_t checkpoint_reserve_milliseconds = 0;
    std::size_t cache_peak_entries = 0, cache_peak_memory = 0;
    double cache_bytes_per_state = 0.0;
    bool cache_saturated = false;
    bool milp_attempted = false, milp_incumbent_accepted = false;
    cutwidth::MilpStatus milp_status = cutwidth::MilpStatus::unknown;
    double milp_runtime = 0.0;
    double milp_model_build = 0.0, milp_solve = 0.0;
    std::int64_t milp_nodes = 0;
    std::optional<double> milp_dual_bound;
    bool sdp_attempted = false, sdp_available = false, sdp_raw_converged = false;
    double sdp_primal_residual = 0.0;
    std::optional<std::uint32_t> sdp_certified_lower_bound;
    double sdp_primal_objective = 0.0, sdp_dual_objective = 0.0;
    double sdp_dual_residual = 0.0, sdp_solve_seconds = 0.0;
    std::size_t sdp_solver_iterations = 0;
    std::size_t sdp_bisection_calls = 0;
    std::size_t sdp_triangle_cuts = 0;
    std::uint64_t sdp_state_requests = 0, sdp_state_certified = 0;
    std::uint64_t sdp_state_prunes = 0, sdp_state_cache_hits = 0;
    std::uint64_t sdp_state_calls = 0, sdp_state_busy = 0;
    std::uint64_t sdp_state_budget_rejections = 0;
    std::uint64_t sdp_state_uncertified = 0, sdp_state_dimension_rejections = 0;
    std::size_t sdp_state_preferred_max_dimension = 0;
    int sdp_solver_status = 0;
    cutwidth::PartialBoundStats bound_stats;
    std::uint32_t root_degree_bound = 0, root_density_bound = 0;
    std::uint32_t root_average_degree_bound = 0;
    std::uint32_t root_grooming_bound = 0;
    std::uint64_t pb_calls = 0, pb_sat_certificates = 0, pb_unsat_certificates = 0;
    cutwidth::pb::DecisionProvenance pb_last;
    bool pb_incremental_attempted = false, pb_incremental_available = false;
    std::uint64_t pb_incremental_calls = 0, pb_incremental_sat = 0;
    std::uint64_t pb_incremental_unsat_exploratory = 0;
    double pb_incremental_seconds = 0.0;
    std::array<std::uint64_t, 5> node_memo_hits{};
    std::uint64_t node_memo_computations = 0, node_memo_prunes = 0;
    std::uint64_t node_memo_child_rejections = 0, node_memo_collisions = 0;
    std::uint64_t node_memo_saturation = 0, node_state_updates = 0,
                  residual_histogram_updates = 0, node_sorts_avoided = 0;
    std::size_t node_memo_memory = 0;
    bool node_memo_available = false;
    std::uint64_t heuristic_interval_evaluations = 0, heuristic_full_fallbacks = 0;
    std::uint64_t heuristic_spectral_seeds = 0, heuristic_grasp_constructions = 0;
    std::uint64_t heuristic_vns_evaluations = 0, heuristic_portfolio_improvements = 0;
    double heuristic_runtime = 0.0, time_to_final_ub = 0.0;
    std::vector<cutwidth::MilestoneSnapshot> milestone_snapshots;
    std::uint64_t controller_events = 0, censored_decisions = 0;
    std::uint64_t adaptive_sessions_created = 0, adaptive_session_resumes = 0;
    std::uint64_t adaptive_session_services = 0;
    double adaptive_dfs_service_seconds = 0.0, controller_overhead_seconds = 0.0;
    std::size_t adaptive_dfs_worker_allocation = 0;
    std::size_t adaptive_incumbent_worker_allocation = 0;
    double allocated_worker_seconds = 0.0, busy_worker_seconds = 0.0;
    double compatibility_wall_time_capacity_seconds = 0.0;
    std::size_t peak_active_physical_leases = 0;
    std::uint64_t useful_leases = 0;
    std::uint64_t empty_claim_exits = 0;
    std::uint64_t cross_session_steals = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> per_epoch_useful_work;
    std::unordered_map<std::uint32_t, std::uint64_t> per_threshold_useful_work;
    std::uint64_t residual_dp_service_calls = 0, residual_dp_states = 0;
    std::size_t residual_dp_projected_bytes = 0;
    bool residual_dp_applicable = false, residual_dp_admitted = false,
         residual_dp_completed = false;
    std::string residual_dp_skip_reason;
    std::uint64_t residual_dp_attempts = 0;
    std::uint64_t residual_dp_admissions = 0;
    std::uint64_t residual_dp_governor_or_cap_rejections = 0;
    std::uint64_t residual_dp_completed_tails = 0;
    std::uint64_t residual_dp_infeasible_prunes = 0;
    std::uint64_t residual_dp_feasible_witnesses = 0;
    std::uint64_t residual_dp_peak_bytes = 0;
    double residual_dp_seconds = 0.0;
    std::uint64_t residual_dp_cold_restarts = 0;
    std::uint64_t pb_sat_root_attempts = 0, pb_sat_root_sat = 0;
    std::uint64_t pb_sat_root_certified_unsat = 0, pb_sat_root_timeouts = 0;
    std::uint64_t pb_sat_root_failures = 0, pb_sat_root_checker_successes = 0;
    std::uint32_t pb_sat_root_active_threshold = 0;
    std::uint64_t pb_sat_root_active_cardinality = 0;
    double pb_sat_root_solver_seconds = 0.0, pb_sat_root_checker_seconds = 0.0;
    std::string pb_sat_root_last_cnf_path, pb_sat_root_last_proof_path;
    std::string pb_sat_root_last_result, pb_sat_root_backend;
    std::string pb_sat_root_provenance, pb_sat_root_proof_hash;
    std::string pb_sat_root_ordering;
    std::uint32_t pb_sat_root_ordering_maximum_frontier = 0;
    std::uint32_t pb_sat_root_ordering_bandwidth = 0;
    std::uint64_t pb_sat_root_ordering_total_edge_span = 0;
    std::uint64_t pb_sat_root_proof_bytes = 0;
    cutwidth::MemoryGovernorStats memory_stats;
    std::uint64_t incumbent_service_calls = 0, incumbent_iterations = 0;
    std::uint64_t incumbent_candidate_evaluations = 0;
    std::uint64_t incumbent_verified_improvements = 0;
    std::uint64_t incumbent_no_progress_bursts = 0;
    double incumbent_service_seconds = 0.0;
    std::uint64_t configured_proof_regions_bound = 0;
    std::uint64_t resolved_proof_regions_bound = 0;
    std::uint64_t peak_proof_regions = 0;
    std::uint64_t suppressed_donations = 0;
    if (cli.engine == "legacy") {
        result = cutwidth::ExactSolver(graph, solver_options).solve();
    } else {
        cutwidth::OptimizerV2Options options;
        options.time_limit = cli.time_limit;
        options.proof_backend = cli.proof_backend;
        options.pb_options = make_pb_options(cli, cli.time_limit);
        options.milp_time_seconds = static_cast<double>(cli.milp_time.count()) / 1000.0;
        options.annealing_min_vertices = cli.annealing_min_vertices;
        options.descending_feasible_steps_before_binary = cli.descending_feasible_steps;
        options.heuristic_evaluation = cli.heuristic_evaluation;
        options.heuristic_tiebreak = cli.heuristic_tiebreak;
        options.heuristic_search = cli.heuristic_search;
        options.heuristic_time = cli.heuristic_time;
        options.sdp_iterations = cli.sdp_iterations;
        options.sdp_max_dimension = cli.sdp_max_dimension;
        options.sdp_backend = cli.sdp_backend;
        options.sdp_time_seconds = static_cast<double>(cli.sdp_time.count()) / 1000.0;
        options.sdp_max_cone_entries = cli.sdp_max_cone_entries;
        options.sdp_bisection_offsets = cli.sdp_bisection_offsets;
        options.sdp_triangle_cuts = cli.sdp_triangle_cuts;
        options.sdp_quantization_bits = cli.sdp_quantization_bits;
        options.cache_mode = cli.cache_mode;
        options.sdp_schedule = cli.sdp_schedule;
        options.sdp_total_time = cli.sdp_total_time;
        options.sdp_max_calls = cli.sdp_max_calls;
        options.sdp_max_state_dimension = cli.sdp_max_state_dimension;
        options.sdp_trigger_nodes = cli.sdp_trigger_nodes;
        apply_features(cli, options);
        options.controller = cli.controller;
        options.threshold_scheduler = cli.threshold_scheduler;
        options.milestones = cli.milestones;
        options.memory_budget_bytes = cli.memory_budget_bytes;
        options.residual_dp_max_bytes = cli.residual_dp_max_bytes;
        options.dfs_residual_dp_max_remaining = cli.dfs_residual_dp_max_remaining;
        options.controller_overhead_fraction = cli.controller_overhead_fraction;
        options.adaptive_arms = cli.adaptive_arms;
        options.checkpoint_out = cli.checkpoint_out;
        options.resume = cli.resume;
        options.strategy_trace = cli.strategy_trace;
        if (cli.controller == cutwidth::ControllerMode::adaptive) {
            auto arm = [&](std::string_view name) {
                return std::find(cli.adaptive_arms.begin(), cli.adaptive_arms.end(), name) !=
                    cli.adaptive_arms.end();
            };
            options.use_partial_bounds = arm("bounds");
            if (arm("alns")) options.heuristic_search = cutwidth::HeuristicSearch::portfolio;
            if (arm("residual-dp") && options.node_memo_depth == 0 &&
                options.node_memo_memory_bytes != 0)
                options.node_memo_depth = 2;
            if (!arm("sdp")) options.sdp_schedule = cutwidth::sdp::SdpSchedule::off;
        }
        if (cli.engine == "v2") {
            options.reuse_failed_state_cache_across_thresholds = false;
        }
        options.pb_sat_root_solver = cli.pb_sat_root_solver;
        options.pb_sat_root_checker = cli.pb_sat_root_checker;
        options.pb_sat_root_dir = cli.pb_sat_root_dir;
        options.pb_sat_root_backend = cli.pb_sat_root_backend;
        options.pb_sat_root_ordering = cli.pb_sat_root_ordering;
        options.pb_sat_root_timeout = cli.pb_sat_root_timeout;
        options.pb_sat_root_q = cli.pb_sat_root_q;
        options.pb_sat_root_max_gap = cli.pb_sat_root_max_gap;

        if (cli.controller == cutwidth::ControllerMode::adaptive) {
            auto arm = [&](std::string_view name) {
                return std::find(cli.adaptive_arms.begin(), cli.adaptive_arms.end(), name) !=
                    cli.adaptive_arms.end();
            };
            if (arm("pb-sat-root")) {
                if (cli.pb_sat_root_backend == cutwidth::PbSatRootBackend::external) {
                    if (cli.pb_sat_root_solver.empty())
                        throw std::invalid_argument("external pb-sat-root solver path must be specified");
                    if (cli.pb_sat_root_checker.empty())
                        throw std::invalid_argument("external pb-sat-root checker path must be specified");
                    if (cli.pb_sat_root_dir.empty())
                        throw std::invalid_argument("external pb-sat-root artifact directory must be specified");
                    if (cli.pb_sat_root_timeout.count() <= 0)
                        throw std::invalid_argument("external pb-sat-root timeout must be positive");
                }
                if (cli.pb_sat_root_q && *cli.pb_sat_root_q > graph.size())
                    throw std::invalid_argument("pb-sat-root q exceeds the graph vertex count");
            }
        }

        const auto v2 = cutwidth::optimize_cutwidth_v2(graph, options);
        result.optimal = v2.optimal;
        result.lower_bound = v2.lower_bound;
        result.upper_bound = v2.upper_bound;
        result.ordering = v2.ordering;
        result.nodes_expanded = v2.stats.nodes_expanded;
        result.pruned_by_bound = v2.stats.children_rejected_by_cut;
        result.pruned_by_dominance = v2.stats.failed_cache_hits;
        result.transposition_table_size = v2.stats.failed_states_recorded;
        v2_decision_calls = v2.stats.decision_calls;
        milestone_snapshots = v2.stats.milestones;
        controller_events = v2.stats.controller_events;
        controller_overhead_seconds = v2.stats.controller_overhead_seconds;
        adaptive_sessions_created = v2.stats.adaptive_sessions_created;
        adaptive_session_resumes = v2.stats.adaptive_session_resumes;
        adaptive_session_services = v2.stats.adaptive_session_services;
        adaptive_dfs_service_seconds = v2.stats.adaptive_dfs_service_seconds;
        adaptive_dfs_worker_allocation = v2.stats.adaptive_dfs_worker_allocation;
        adaptive_incumbent_worker_allocation =
            v2.stats.adaptive_incumbent_worker_allocation;
        allocated_worker_seconds = v2.stats.allocated_worker_seconds;
        busy_worker_seconds = v2.stats.busy_worker_seconds;
        compatibility_wall_time_capacity_seconds = v2.stats.compatibility_wall_time_capacity_seconds;
        peak_active_physical_leases = v2.stats.peak_active_physical_leases;
        useful_leases = v2.stats.useful_leases;
        empty_claim_exits = v2.stats.empty_claim_exits;
        cross_session_steals = v2.stats.cross_session_steals;
        per_epoch_useful_work = v2.stats.per_epoch_useful_work;
        per_threshold_useful_work = v2.stats.per_threshold_useful_work;
        residual_dp_service_calls = v2.stats.residual_dp_service_calls;
        residual_dp_states = v2.stats.residual_dp_states;
        residual_dp_projected_bytes = v2.stats.residual_dp_projected_bytes;
        residual_dp_applicable = v2.stats.residual_dp_applicable;
        residual_dp_admitted = v2.stats.residual_dp_admitted;
        residual_dp_skip_reason = v2.stats.residual_dp_skip_reason;
        residual_dp_completed = v2.stats.residual_dp_completed;
        residual_dp_attempts = v2.stats.residual_dp_attempts;
        residual_dp_admissions = v2.stats.residual_dp_admissions;
        residual_dp_governor_or_cap_rejections = v2.stats.residual_dp_governor_or_cap_rejections;
        residual_dp_completed_tails = v2.stats.residual_dp_completed_tails;
        residual_dp_infeasible_prunes = v2.stats.residual_dp_infeasible_prunes;
        residual_dp_feasible_witnesses = v2.stats.residual_dp_feasible_witnesses;
        residual_dp_peak_bytes = v2.stats.residual_dp_peak_bytes;
        residual_dp_seconds = v2.stats.residual_dp_seconds;
        residual_dp_cold_restarts = v2.stats.residual_dp_cold_restarts;
        pb_sat_root_attempts = v2.stats.pb_sat_root_attempts;
        pb_sat_root_sat = v2.stats.pb_sat_root_sat;
        pb_sat_root_certified_unsat = v2.stats.pb_sat_root_certified_unsat;
        pb_sat_root_timeouts = v2.stats.pb_sat_root_timeouts;
        pb_sat_root_failures = v2.stats.pb_sat_root_failures;
        pb_sat_root_checker_successes = v2.stats.pb_sat_root_checker_successes;
        pb_sat_root_active_threshold = v2.stats.pb_sat_root_active_threshold;
        pb_sat_root_active_cardinality = v2.stats.pb_sat_root_active_cardinality;
        pb_sat_root_solver_seconds = v2.stats.pb_sat_root_solver_seconds;
        pb_sat_root_checker_seconds = v2.stats.pb_sat_root_checker_seconds;
        pb_sat_root_last_cnf_path = v2.stats.pb_sat_root_last_cnf_path;
        pb_sat_root_last_proof_path = v2.stats.pb_sat_root_last_proof_path;
        pb_sat_root_last_result = v2.stats.pb_sat_root_last_result;
        pb_sat_root_backend = v2.stats.pb_sat_root_backend;
        pb_sat_root_provenance = v2.stats.pb_sat_root_provenance;
        pb_sat_root_proof_bytes = v2.stats.pb_sat_root_proof_bytes;
        pb_sat_root_proof_hash = v2.stats.pb_sat_root_proof_hash;
        pb_sat_root_ordering = v2.stats.pb_sat_root_ordering;
        pb_sat_root_ordering_maximum_frontier = v2.stats.pb_sat_root_ordering_maximum_frontier;
        pb_sat_root_ordering_bandwidth = v2.stats.pb_sat_root_ordering_bandwidth;
        pb_sat_root_ordering_total_edge_span = v2.stats.pb_sat_root_ordering_total_edge_span;
        memory_stats = v2.stats.memory;
        censored_decisions = v2.stats.censored_decisions;
        incumbent_service_calls = v2.stats.incumbent_service_calls;
        incumbent_iterations = v2.stats.incumbent_iterations;
        incumbent_candidate_evaluations = v2.stats.incumbent_candidate_evaluations;
        incumbent_verified_improvements = v2.stats.incumbent_verified_improvements;
        incumbent_no_progress_bursts = v2.stats.incumbent_no_progress_bursts;
        incumbent_service_seconds = v2.stats.incumbent_service_seconds;
        parallel_workers_used = v2.stats.parallel_workers_used;
        parallel_root_tasks_started = v2.stats.parallel_root_tasks_started;
        parallel_root_tasks_completed = v2.stats.parallel_root_tasks_completed;
        v2_twin_skips = v2.stats.twin_symmetric_children_skipped;
        best_next_bucket_checks = v2.stats.best_next_bucket_checks;
        best_next_bucket_parent_prunes = v2.stats.best_next_bucket_parent_prunes;
        best_next_bucket_candidates_avoided = v2.stats.best_next_bucket_candidates_avoided;
        candidate_scan_checks = v2.stats.candidate_scan_checks;
        candidate_index_gathers = v2.stats.candidate_index_gathers;
        candidate_index_bucket_slots_visited =
            v2.stats.candidate_index_bucket_slots_visited;
        candidate_index_vertices_emitted = v2.stats.candidate_index_vertices_emitted;
        candidate_index_forward_updates = v2.stats.candidate_index_forward_updates;
        candidate_index_rollback_updates = v2.stats.candidate_index_rollback_updates;
        candidate_index_cross_checks = v2.stats.candidate_index_cross_checks;
        local_continuation_calls = v2.stats.local_continuation_calls;
        local_continuation_slack_gate_skips =
            v2.stats.local_continuation_slack_gate_skips;
        local_continuation_branch_gate_skips =
            v2.stats.local_continuation_branch_gate_skips;
        local_continuation_inconclusive = v2.stats.local_continuation_inconclusive;
        local_continuation_states = v2.stats.local_continuation_states;
        local_continuation_parent_prunes = v2.stats.local_continuation_parent_prunes;
        local_continuation_nanoseconds = v2.stats.local_continuation_nanoseconds;
        local_continuation_cross_checks = v2.stats.local_continuation_cross_checks;
        lookahead_checks = v2.stats.depth_two_lookahead_checks;
        lookahead_rejections = v2.stats.children_rejected_by_depth_two_lookahead;
        cache_strengthenings = v2.stats.cache_strengthenings;
        cache_collisions = v2.stats.cache_collisions;
        cache_segment_growths = v2.stats.cache_segment_growths;
        cache_lookup_probes = v2.stats.cache_lookup_probes;
        cache_insertion_probes = v2.stats.cache_insertion_probes;
        cache_probes_avoided_after_saturation =
            v2.stats.cache_probes_avoided_after_saturation;
        cache_page_promotions = v2.stats.cache_page_promotions;
        cache_page_second_chances = v2.stats.cache_page_second_chances;
        cache_pages_recycled = v2.stats.cache_pages_recycled;
        cache_replacement_admissions = v2.stats.cache_replacement_admissions;
        cache_entries_evicted = v2.stats.cache_entries_evicted;
        cache_evicted_depth_sum = v2.stats.cache_evicted_depth_sum;
        cache_maximum_evicted_depth = v2.stats.cache_maximum_evicted_depth;
        unique_canonical_claims = v2.stats.unique_canonical_claims;
        duplicate_ownership_waits = v2.stats.duplicate_ownership_waits;
        ownership_saturation = v2.stats.ownership_saturation;
        resumed_from_checkpoint = v2.stats.resumed_from_checkpoint;
        checkpoint_elapsed_milliseconds = v2.stats.checkpoint_elapsed_milliseconds;
        checkpoints_written = v2.stats.checkpoints_written;
        checkpoint_write_seconds = v2.stats.checkpoint_write_seconds;
        checkpoint_reserve_milliseconds = v2.stats.checkpoint_reserve_milliseconds;
        cache_peak_entries = v2.stats.cache_peak_entries;
        cache_peak_memory = v2.stats.cache_peak_memory_bytes;
        cache_bytes_per_state = v2.stats.cache_bytes_per_state;
        cache_saturated = v2.stats.cache_saturated;
        node_memo_hits = v2.stats.node_memo_hits_by_depth;
        node_memo_computations = v2.stats.node_memo_computations;
        node_memo_prunes = v2.stats.node_memo_prunes;
        node_memo_child_rejections = v2.stats.node_memo_child_rejections;
        node_memo_collisions = v2.stats.node_memo_collisions;
        node_memo_saturation = v2.stats.node_memo_saturation;
        node_memo_memory = v2.stats.node_memo_memory_bytes;
        node_memo_available = v2.stats.node_memo_available;
        node_state_updates = v2.stats.node_state_updates;
        residual_histogram_updates = v2.stats.residual_histogram_updates;
        node_sorts_avoided = v2.stats.node_sorts_avoided;
        heuristic_interval_evaluations = v2.stats.heuristic_interval_evaluations;
        heuristic_full_fallbacks = v2.stats.heuristic_full_fallbacks;
        heuristic_runtime = v2.stats.heuristic_runtime_seconds;
        time_to_final_ub = v2.stats.time_to_final_upper_bound_seconds;
        heuristic_spectral_seeds = v2.stats.heuristic_spectral_seeds;
        heuristic_grasp_constructions = v2.stats.heuristic_grasp_constructions;
        heuristic_vns_evaluations = v2.stats.heuristic_vns_evaluations;
        heuristic_portfolio_improvements = v2.stats.heuristic_portfolio_improvements;
        bound_stats = v2.stats.partial_bounds;
        root_degree_bound = v2.stats.root_degree_bound;
        root_density_bound = v2.stats.root_density_bound;
        root_average_degree_bound = v2.stats.root_average_degree_bound;
        root_grooming_bound = v2.stats.root_grooming_bound;
        milp_attempted = v2.stats.milp_attempted;
        milp_status = v2.stats.milp_status;
        milp_runtime = v2.stats.milp_runtime_seconds;
        milp_model_build = v2.stats.milp_model_build_seconds;
        milp_solve = v2.stats.milp_solve_seconds;
        milp_nodes = v2.stats.milp_nodes;
        milp_dual_bound = v2.stats.milp_diagnostic_dual_bound;
        milp_incumbent_accepted = v2.stats.milp_incumbent_accepted;
        sdp_attempted = v2.stats.sdp_attempted;
        sdp_available = v2.stats.sdp_available;
        sdp_raw_converged = v2.stats.sdp_raw_converged;
        sdp_primal_residual = v2.stats.sdp_primal_residual;
        sdp_certified_lower_bound = v2.stats.sdp_certified_lower_bound;
        sdp_primal_objective = v2.stats.sdp_primal_objective;
        sdp_dual_objective = v2.stats.sdp_dual_objective;
        sdp_dual_residual = v2.stats.sdp_dual_residual;
        sdp_solve_seconds = v2.stats.sdp_solve_seconds;
        sdp_solver_iterations = v2.stats.sdp_solver_iterations;
        sdp_bisection_calls = v2.stats.sdp_bisection_calls;
        sdp_triangle_cuts = v2.stats.sdp_triangle_cuts;
        sdp_solver_status = v2.stats.sdp_solver_status;
        sdp_state_requests = v2.stats.sdp_state_requests;
        sdp_state_certified = v2.stats.sdp_state_certified;
        sdp_state_prunes = v2.stats.sdp_state_prunes;
        sdp_state_cache_hits = v2.stats.sdp_state_cache_hits;
        sdp_state_calls = v2.stats.sdp_state_calls;
        sdp_state_busy = v2.stats.sdp_state_busy;
        sdp_state_budget_rejections = v2.stats.sdp_state_budget_rejections;
        sdp_state_uncertified = v2.stats.sdp_state_uncertified;
        sdp_state_dimension_rejections = v2.stats.sdp_state_dimension_rejections;
        sdp_state_preferred_max_dimension = v2.stats.sdp_state_preferred_max_dimension;
        pb_calls = v2.stats.pb_calls;
        pb_sat_certificates = v2.stats.pb_sat_certificates;
        pb_unsat_certificates = v2.stats.pb_unsat_certificates;
        pb_last = v2.stats.pb_last;
        pb_incremental_attempted = v2.stats.pb_incremental_attempted;
        pb_incremental_available = v2.stats.pb_incremental_available;
        pb_incremental_calls = v2.stats.pb_incremental_calls;
        pb_incremental_sat = v2.stats.pb_incremental_sat;
        pb_incremental_unsat_exploratory =
            v2.stats.pb_incremental_unsat_exploratory;
        pb_incremental_seconds = v2.stats.pb_incremental_seconds;
        configured_proof_regions_bound = v2.stats.configured_proof_regions_bound;
        resolved_proof_regions_bound = v2.stats.resolved_proof_regions_bound;
        peak_proof_regions = v2.stats.peak_proof_regions;
        suppressed_donations = v2.stats.suppressed_donations;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    bool verified = false;
    if (cli.verify) {
        if (!graph.validate_ordering(result.ordering)) {
            throw std::runtime_error("verification failed: result is not a vertex permutation");
        }
        const auto recomputed = graph.ordering_cutwidth(result.ordering);
        if (recomputed != result.upper_bound) {
            throw std::runtime_error("verification failed: reported upper bound does not match ordering");
        }
        if (result.optimal && result.lower_bound != result.upper_bound) {
            throw std::runtime_error("verification failed: optimal result has unequal bounds");
        }
        verified = true;
    }

    if (cli.json) {
        std::cout << '{'
                  << "\"schema_version\":3,"
                  << "\"engine\":\"" << engine << "\","
                  << "\"status\":\"" << (result.optimal ? "OPTIMAL" : "FEASIBLE") << "\","
                  << "\"vertices\":" << graph.size() << ','
                  << "\"edges\":" << graph.edge_count() << ','
                  << "\"lower_bound\":" << result.lower_bound << ','
                  << "\"upper_bound\":" << result.upper_bound << ','
                  << "\"ordering\":";
        print_json_array(std::cout, result.ordering,
                         [&](std::size_t vertex) { print_json_string(std::cout, graph.label(vertex)); });
        std::cout << ",\"runtime_seconds\":" << std::fixed << std::setprecision(6) << elapsed
                  << ",\"nodes_expanded\":" << result.nodes_expanded
                  << ",\"pruned_by_bound\":" << result.pruned_by_bound
                  << ",\"pruned_by_dominance\":" << result.pruned_by_dominance
                  << ",\"transposition_table_size\":" << result.transposition_table_size
                  << ",\"decision_calls\":" << v2_decision_calls
                  << ",\"controller\":\""
                  << (cli.controller == cutwidth::ControllerMode::adaptive ? "adaptive" : "static")
                  << "\",\"threshold_scheduler\":\""
                  << (cli.threshold_scheduler == cutwidth::ThresholdSchedulerMode::value_aware ? "value-aware" :
                      (cli.threshold_scheduler == cutwidth::ThresholdSchedulerMode::primary_first ? "primary-first" : "recurrence"))
                  << "\",\"controller_events\":" << controller_events
                  << ",\"controller_overhead_seconds\":" << controller_overhead_seconds
                  << ",\"adaptive_sessions_created\":" << adaptive_sessions_created
                  << ",\"adaptive_session_resumes\":" << adaptive_session_resumes
                  << ",\"adaptive_session_services\":" << adaptive_session_services
                  << ",\"adaptive_dfs_service_seconds\":" << adaptive_dfs_service_seconds
                  << ",\"adaptive_dfs_worker_allocation\":"
                  << adaptive_dfs_worker_allocation
                  << ",\"adaptive_incumbent_worker_allocation\":"
                  << adaptive_incumbent_worker_allocation
                  << ",\"residual_dp_service_calls\":" << residual_dp_service_calls
                  << ",\"residual_dp_states\":" << residual_dp_states
                  << ",\"residual_dp_projected_bytes\":" << residual_dp_projected_bytes
                  << ",\"residual_dp_applicable\":" << (residual_dp_applicable ? "true" : "false")
                  << ",\"residual_dp_admitted\":" << (residual_dp_admitted ? "true" : "false")
                  << ",\"residual_dp_skip_reason\":\"" << residual_dp_skip_reason << "\""
                  << ",\"residual_dp_completed\":" << (residual_dp_completed ? "true" : "false")
                  << ",\"residual_dp_attempts\":" << residual_dp_attempts
                  << ",\"residual_dp_admissions\":" << residual_dp_admissions
                  << ",\"residual_dp_rejections\":" << residual_dp_governor_or_cap_rejections
                  << ",\"residual_dp_completed_tails\":" << residual_dp_completed_tails
                  << ",\"residual_dp_infeasible_prunes\":" << residual_dp_infeasible_prunes
                  << ",\"residual_dp_feasible_witnesses\":" << residual_dp_feasible_witnesses
                  << ",\"residual_dp_peak_bytes\":" << residual_dp_peak_bytes
                  << ",\"residual_dp_seconds\":" << std::fixed << std::setprecision(6) << residual_dp_seconds
                  << ",\"residual_dp_cold_restarts\":" << residual_dp_cold_restarts
                  << ",\"pb_sat_root_attempts\":" << pb_sat_root_attempts
                  << ",\"pb_sat_root_sat\":" << pb_sat_root_sat
                  << ",\"pb_sat_root_certified_unsat\":" << pb_sat_root_certified_unsat
                  << ",\"pb_sat_root_timeouts\":" << pb_sat_root_timeouts
                  << ",\"pb_sat_root_failures\":" << pb_sat_root_failures
                  << ",\"pb_sat_root_checker_successes\":" << pb_sat_root_checker_successes
                  << ",\"pb_sat_root_active_threshold\":" << pb_sat_root_active_threshold
                  << ",\"pb_sat_root_active_cardinality\":" << pb_sat_root_active_cardinality
                  << ",\"pb_sat_root_solver_seconds\":" << std::fixed << std::setprecision(6) << pb_sat_root_solver_seconds
                  << ",\"pb_sat_root_checker_seconds\":" << std::fixed << std::setprecision(6) << pb_sat_root_checker_seconds
                  << ",\"pb_sat_root_last_cnf_path\":" << std::quoted(pb_sat_root_last_cnf_path)
                  << ",\"pb_sat_root_last_proof_path\":" << std::quoted(pb_sat_root_last_proof_path)
                  << ",\"pb_sat_root_last_result\":" << std::quoted(pb_sat_root_last_result)
                  << ",\"pb_sat_root_backend\":" << std::quoted(pb_sat_root_backend)
                  << ",\"pb_sat_root_provenance\":" << std::quoted(pb_sat_root_provenance)
                  << ",\"pb_sat_root_proof_bytes\":" << pb_sat_root_proof_bytes
                  << ",\"pb_sat_root_proof_hash\":" << std::quoted(pb_sat_root_proof_hash)
                  << ",\"pb_sat_root_ordering\":" << std::quoted(pb_sat_root_ordering)
                  << ",\"pb_sat_root_ordering_maximum_frontier\":" << pb_sat_root_ordering_maximum_frontier
                  << ",\"pb_sat_root_ordering_bandwidth\":" << pb_sat_root_ordering_bandwidth
                  << ",\"pb_sat_root_ordering_total_edge_span\":" << pb_sat_root_ordering_total_edge_span
                  << ",\"memory_budget_bytes\":" << memory_stats.budget_bytes
                  << ",\"memory_baseline_rss_bytes\":" << memory_stats.baseline_rss_bytes
                  << ",\"memory_sampled_rss_bytes\":" << memory_stats.sampled_rss_bytes
                  << ",\"memory_committed_lease_bytes\":" << memory_stats.committed_lease_bytes
                  << ",\"memory_untracked_bytes\":" << memory_stats.untracked_bytes
                  << ",\"memory_pressure\":" << (memory_stats.memory_pressure ? "true" : "false")
                  << ",\"censored_decisions\":" << censored_decisions
                  << ",\"incumbent_service_calls\":" << incumbent_service_calls
                  << ",\"incumbent_iterations\":" << incumbent_iterations
                  << ",\"incumbent_candidate_evaluations\":"
                  << incumbent_candidate_evaluations
                  << ",\"incumbent_verified_improvements\":"
                  << incumbent_verified_improvements
                  << ",\"incumbent_no_progress_bursts\":"
                  << incumbent_no_progress_bursts
                  << ",\"incumbent_service_seconds\":" << incumbent_service_seconds
                  << ",\"milestones\":[";
        for (std::size_t i = 0; i < milestone_snapshots.size(); ++i) {
            if (i) std::cout << ',';
            const auto& snapshot = milestone_snapshots[i];
            std::cout << "{\"scheduled_milliseconds\":"
                      << snapshot.scheduled_milliseconds
                      << ",\"elapsed_milliseconds\":" << snapshot.elapsed_milliseconds
                      << ",\"lower_bound\":" << snapshot.lower_bound
                      << ",\"upper_bound\":" << snapshot.upper_bound
                      << ",\"nodes_expanded\":" << snapshot.nodes_expanded
                      << ",\"decision_calls\":" << snapshot.decision_calls
                      << ",\"allocated_worker_seconds\":"
                      << snapshot.allocated_worker_seconds
                      << ",\"busy_worker_seconds\":" << snapshot.busy_worker_seconds
                      << ",\"worker_utilization\":"
                      << (snapshot.allocated_worker_seconds == 0.0 ? 0.0 :
                          snapshot.busy_worker_seconds / snapshot.allocated_worker_seconds)
                      << ",\"controller_overhead_seconds\":"
                      << snapshot.controller_overhead_seconds
                      << ",\"optimal\":" << (snapshot.optimal ? "true" : "false") << '}';
        }
        std::cout << ']'
                  << ",\"parallel_workers_used\":" << parallel_workers_used
                  << ",\"allocated_worker_seconds\":" << allocated_worker_seconds
                  << ",\"busy_worker_seconds\":" << busy_worker_seconds
                  << ",\"worker_utilization\":"
                  << (allocated_worker_seconds == 0.0 ? 0.0 :
                      busy_worker_seconds / allocated_worker_seconds)
                  << ",\"parallel_runtime\":\""
                  << (cli.parallel_runtime == cutwidth::ParallelRuntime::onetbb ?
                          "onetbb" : "native") << "\""
                  << ",\"parallel_root_tasks_started\":"
                  << parallel_root_tasks_started
                  << ",\"parallel_root_tasks_completed\":"
                  << parallel_root_tasks_completed
                  << ",\"heuristic_search\":\""
                  << (cli.heuristic_search == cutwidth::HeuristicSearch::portfolio ?
                          "portfolio" : "basic") << '"'
                  << ",\"heuristic_budget_seconds\":"
                  << static_cast<double>(cli.heuristic_time.count()) / 1000.0
                  << ",\"proof_backend\":\""
                  << (cli.proof_backend == cutwidth::ProofBackend::pb ? "pb" : "dfs") << '"'
                  << ",\"pb_calls\":" << pb_calls
                  << ",\"pb_native_incremental\":"
                  << (cli.pb_native_incremental ? "true" : "false")
                  << ",\"pb_sat_certificates\":" << pb_sat_certificates
                  << ",\"pb_unsat_certificates\":" << pb_unsat_certificates
                  << ",\"pb_incremental_attempted\":"
                  << (pb_incremental_attempted ? "true" : "false")
                  << ",\"pb_incremental_available\":"
                  << (pb_incremental_available ? "true" : "false")
                  << ",\"pb_incremental_calls\":" << pb_incremental_calls
                  << ",\"pb_incremental_sat\":" << pb_incremental_sat
                  << ",\"pb_incremental_unsat_exploratory\":"
                  << pb_incremental_unsat_exploratory
                  << ",\"pb_incremental_seconds\":" << pb_incremental_seconds
                  << ",\"pb_solver\":\"" << cutwidth::pb::solver_name(cli.pb_solver) << '"'
                  << ",\"pb_encoding\":\"" << cutwidth::pb::encoding_name(cli.pb_encoding) << '"'
                  << ",\"pb_position_channeling\":"
                  << (pb_last.position_channeling ? "true" : "false")
                  << ",\"pb_first_vertex_split\":"
                  << (pb_last.first_vertex_split ? "true" : "false")
                  << ",\"pb_branches_total\":" << pb_last.branches_total
                  << ",\"pb_branches_completed\":" << pb_last.branches_completed
                  << ",\"pb_branches_unsat_verified\":"
                  << pb_last.branches_unsat_verified
                  << ",\"pb_branches_sat_verified\":" << pb_last.branches_sat_verified
                  << ",\"pb_branches_unsat_unverified\":"
                  << pb_last.branches_unsat_unverified
                  << ",\"pb_branches_timed_out\":" << pb_last.branches_timed_out
                  << ",\"pb_branches_other_failures\":"
                  << pb_last.branches_other_failures
                  << ",\"pb_branches_with_proof\":" << pb_last.branches_with_proof
                  << ",\"pb_status\":\"" << cutwidth::pb::status_name(pb_last.external_status) << '"'
                  << ",\"pb_model_hash\":";
        if (pb_calls != 0) print_json_string(std::cout, pb_last.model_hash);
        else std::cout << "null";
        std::cout << ",\"pb_variables\":" << pb_last.variables
                  << ",\"pb_clauses\":" << pb_last.clauses
                  << ",\"pb_proof_generated\":" << (pb_last.proof_generated ? "true" : "false")
                  << ",\"pb_proof_checked\":" << (pb_last.proof_checked ? "true" : "false")
                  << ",\"pb_proof_bytes\":" << pb_last.proof_bytes
                  << ",\"pb_proof_hash\":";
        if (!pb_last.proof_hash.empty()) print_json_string(std::cout, pb_last.proof_hash);
        else std::cout << "null";
        std::cout << ",\"pb_artifact_directory\":";
        if (!pb_last.artifact_directory.empty())
            print_json_string(std::cout, pb_last.artifact_directory);
        else std::cout << "null";
        std::cout
                  << ",\"pb_witness_verified\":" << (pb_last.witness_verified ? "true" : "false")
                  << ",\"pb_solver_version\":";
        if (pb_calls != 0) print_json_string(std::cout, pb_last.solver_version);
        else std::cout << "null";
        std::cout
                  << ",\"cache_mode\":\""
                  << (cli.cache_mode == cutwidth::CacheMode::automatic ? "auto" :
                      cli.cache_mode == cutwidth::CacheMode::cross_threshold ?
                      "cross-threshold" : "fixed-threshold") << '"'
                  << ",\"cache_replacement\":\""
                  << (cli.cache_replacement == cutwidth::CacheReplacementPolicy::freeze
                          ? "freeze" : "generational-clock") << '"'
                  << ",\"cache_replacement_page_capacity\":"
                  << cli.cache_replacement_page_capacity
                  << ",\"effective_cache_mode\":\""
                  << (cli.controller == cutwidth::ControllerMode::adaptive
                          ? "fixed-threshold" :
                      cli.cache_mode == cutwidth::CacheMode::automatic ? "auto" :
                      cli.cache_mode == cutwidth::CacheMode::cross_threshold ?
                          "cross-threshold" : "fixed-threshold") << '"'
                  << ",\"milp_attempted\":" << (milp_attempted ? "true" : "false")
                  << ",\"milp_status\":\"" << milp_status_name(milp_status) << '"'
                  << ",\"milp_runtime_seconds\":" << milp_runtime
                  << ",\"milp_model_build_seconds\":" << milp_model_build
                  << ",\"milp_solve_seconds\":" << milp_solve
                  << ",\"milp_nodes\":" << milp_nodes
                  << ",\"milp_diagnostic_dual_bound\":";
        if (milp_dual_bound) std::cout << *milp_dual_bound;
        else std::cout << "null";
        std::cout
                  << ",\"milp_incumbent_accepted\":" << (milp_incumbent_accepted ? "true" : "false")
                  << ",\"sdp_attempted\":" << (sdp_attempted ? "true" : "false")
                  << ",\"sdp_available\":" << (sdp_available ? "true" : "false")
                  << ",\"sdp_raw_converged\":" << (sdp_raw_converged ? "true" : "false")
                  << ",\"sdp_primal_residual\":" << sdp_primal_residual
                  << ",\"sdp_certified_lower_bound\":";
        if (sdp_certified_lower_bound) std::cout << *sdp_certified_lower_bound;
        else std::cout << "null";
        std::cout
                  << ",\"sdp_schedule\":\""
                  << (cli.sdp_schedule == cutwidth::sdp::SdpSchedule::off ? "off" :
                      cli.sdp_schedule == cutwidth::sdp::SdpSchedule::root ? "root" : "adaptive")
                  << "\",\"sdp_state_requests\":" << sdp_state_requests
                  << ",\"sdp_state_certified\":" << sdp_state_certified
                  << ",\"sdp_state_prunes\":" << sdp_state_prunes
                  << ",\"sdp_state_cache_hits\":" << sdp_state_cache_hits
                  << ",\"sdp_state_calls\":" << sdp_state_calls
                  << ",\"sdp_state_busy\":" << sdp_state_busy
                  << ",\"sdp_state_budget_rejections\":" << sdp_state_budget_rejections
                  << ",\"sdp_state_uncertified\":" << sdp_state_uncertified
                  << ",\"sdp_state_dimension_rejections\":" << sdp_state_dimension_rejections
                  << ",\"sdp_state_preferred_max_dimension\":"
                  << sdp_state_preferred_max_dimension
                  << ",\"sdp\":{\"backend\":\""
                  << (cli.sdp_backend == cutwidth::SdpBackend::clarabel ? "clarabel" :
                      cli.sdp_backend == cutwidth::SdpBackend::clarabel_bisection ?
                          "clarabel-bisection" : "admm")
                  << "\",\"status\":\"" << clarabel_sdp_status_name(sdp_solver_status)
                  << "\",\"status_code\":" << sdp_solver_status
                  << ",\"primal_objective\":" << sdp_primal_objective
                  << ",\"dual_objective\":" << sdp_dual_objective
                  << ",\"primal_residual\":" << sdp_primal_residual
                  << ",\"dual_residual\":" << sdp_dual_residual
                  << ",\"iterations\":" << sdp_solver_iterations
                  << ",\"bisection_calls\":" << sdp_bisection_calls
                  << ",\"triangle_cuts\":" << sdp_triangle_cuts
                  << ",\"solve_seconds\":" << sdp_solve_seconds
                  << ",\"certified_lower_bound\":";
        if (sdp_certified_lower_bound) std::cout << *sdp_certified_lower_bound;
        else std::cout << "null";
        std::cout << '}'
                  << ",\"best_next_bucket_checks\":" << best_next_bucket_checks
                  << ",\"best_next_bucket_parent_prunes\":" << best_next_bucket_parent_prunes
                  << ",\"best_next_bucket_candidates_avoided\":" << best_next_bucket_candidates_avoided
                  << ",\"candidate_enumerator\":\""
                  << candidate_enumerator_name(cli.candidate_enumerator) << '"'
                  << ",\"candidate_scan_checks\":" << candidate_scan_checks
                  << ",\"candidate_index_gathers\":" << candidate_index_gathers
                  << ",\"candidate_index_bucket_slots_visited\":"
                  << candidate_index_bucket_slots_visited
                  << ",\"candidate_index_vertices_emitted\":"
                  << candidate_index_vertices_emitted
                  << ",\"candidate_index_forward_updates\":"
                  << candidate_index_forward_updates
                  << ",\"candidate_index_rollback_updates\":"
                  << candidate_index_rollback_updates
                  << ",\"candidate_index_cross_checks\":"
                  << candidate_index_cross_checks
                  << ",\"local_continuation_calls\":" << local_continuation_calls
                  << ",\"local_continuation_slack_gate_skips\":"
                  << local_continuation_slack_gate_skips
                  << ",\"local_continuation_branch_gate_skips\":"
                  << local_continuation_branch_gate_skips
                  << ",\"local_continuation_inconclusive\":"
                  << local_continuation_inconclusive
                  << ",\"local_continuation_states\":" << local_continuation_states
                  << ",\"local_continuation_parent_prunes\":"
                  << local_continuation_parent_prunes
                  << ",\"local_continuation_nanoseconds\":"
                  << local_continuation_nanoseconds
                  << ",\"local_continuation_cross_checks\":"
                  << local_continuation_cross_checks
                  << ",\"twin_symmetric_children_skipped\":" << v2_twin_skips
                  << ",\"depth_two_lookahead_checks\":" << lookahead_checks
                  << ",\"depth_two_lookahead_rejections\":" << lookahead_rejections
                  << ",\"cache_strengthenings\":" << cache_strengthenings
                  << ",\"cache_collisions\":" << cache_collisions
                  << ",\"cache_segment_growths\":" << cache_segment_growths
                  << ",\"cache_lookup_probes\":" << cache_lookup_probes
                  << ",\"cache_insertion_probes\":" << cache_insertion_probes
                  << ",\"cache_probes_avoided_after_saturation\":"
                  << cache_probes_avoided_after_saturation
                  << ",\"cache_page_promotions\":" << cache_page_promotions
                  << ",\"cache_page_second_chances\":" << cache_page_second_chances
                  << ",\"cache_pages_recycled\":" << cache_pages_recycled
                  << ",\"cache_replacement_admissions\":" << cache_replacement_admissions
                  << ",\"cache_entries_evicted\":" << cache_entries_evicted
                  << ",\"cache_evicted_depth_sum\":" << cache_evicted_depth_sum
                  << ",\"cache_maximum_evicted_depth\":" << cache_maximum_evicted_depth
                  << ",\"unique_canonical_claims\":" << unique_canonical_claims
                  << ",\"duplicate_ownership_waits\":" << duplicate_ownership_waits
                  << ",\"ownership_saturation\":" << ownership_saturation
                  << ",\"resumed_from_checkpoint\":"
                  << (resumed_from_checkpoint ? "true" : "false")
                  << ",\"checkpoint_elapsed_milliseconds\":"
                  << checkpoint_elapsed_milliseconds
                  << ",\"checkpoints_written\":" << checkpoints_written
                  << ",\"checkpoint_write_seconds\":" << checkpoint_write_seconds
                  << ",\"checkpoint_reserve_milliseconds\":"
                  << checkpoint_reserve_milliseconds
                  << ",\"cache_peak_entries\":" << cache_peak_entries
                  << ",\"cache_peak_memory_bytes\":" << cache_peak_memory
                  << ",\"cache_bytes_per_state\":" << cache_bytes_per_state
                  << ",\"cache_saturated\":" << (cache_saturated ? "true" : "false")
                  << ",\"node_memo_available\":" << (node_memo_available ? "true" : "false")
                  << ",\"node_memo_hits_by_depth\":[";
        for (std::size_t d = 0; d < node_memo_hits.size(); ++d) {
            if (d) std::cout << ',';
            std::cout << node_memo_hits[d];
        }
        std::cout << "]"
                  << ",\"node_memo_computations\":" << node_memo_computations
                  << ",\"node_memo_prunes\":" << node_memo_prunes
                  << ",\"node_memo_child_rejections\":" << node_memo_child_rejections
                  << ",\"node_memo_collisions\":" << node_memo_collisions
                  << ",\"node_memo_saturation\":" << node_memo_saturation
                  << ",\"node_memo_memory_bytes\":" << node_memo_memory
                  << ",\"node_state_updates\":" << node_state_updates
                  << ",\"residual_histogram_updates\":"
                  << residual_histogram_updates
                  << ",\"node_sorts_avoided\":" << node_sorts_avoided
                  << ",\"heuristic_interval_evaluations\":" << heuristic_interval_evaluations
                  << ",\"heuristic_full_fallbacks\":" << heuristic_full_fallbacks
                  << ",\"heuristic_runtime_seconds\":" << heuristic_runtime
                  << ",\"time_to_final_upper_bound_seconds\":" << time_to_final_ub
                  << ",\"heuristic_spectral_seeds\":" << heuristic_spectral_seeds
                  << ",\"heuristic_grasp_constructions\":" << heuristic_grasp_constructions
                  << ",\"heuristic_vns_evaluations\":" << heuristic_vns_evaluations
                  << ",\"heuristic_portfolio_improvements\":"
                  << heuristic_portfolio_improvements
                  << ",\"partial_bound_evaluations\":" << bound_stats.evaluations
                  << ",\"lagrangian_evaluations\":" << bound_stats.lagrangian_evaluations
                  << ",\"lagrangian_mincuts\":" << bound_stats.lagrangian_mincuts
                  << ",\"lagrangian_certified_prunes\":" << bound_stats.lagrangian_certified_prunes
                  << ",\"lagrangian_slack_gate_skips\":" << bound_stats.lagrangian_slack_gate_skips
                  << ",\"lagrangian_residual_gate_skips\":" << bound_stats.lagrangian_residual_gate_skips
                  << ",\"lagrangian_ineligible_gate_skips\":" << bound_stats.lagrangian_ineligible_gate_skips
                  << ",\"lagrangian_overflow_gate_skips\":" << bound_stats.lagrangian_overflow_gate_skips
                  << ",\"residual_degree_session_ceiling_skips\":"
                  << bound_stats.residual_degree_session_ceiling_skips
                  << ",\"degeneracy_session_ceiling_skips\":"
                  << bound_stats.degeneracy_session_ceiling_skips
                  << ",\"partial_bound_slack_gate_skips\":"
                  << bound_stats.expensive_slack_gate_skips
                  << ",\"root_degree_bound\":" << root_degree_bound
                  << ",\"root_density_bound\":" << root_density_bound
                  << ",\"root_average_degree_bound\":" << root_average_degree_bound
                  << ",\"root_grooming_bound\":" << root_grooming_bound
                  << ",\"residual_degree_prunes\":" << bound_stats.residual_degree_prunes
                  << ",\"edge_area_prunes\":" << bound_stats.edge_distance_area_prunes
                  << ",\"degree_area_prunes\":" << bound_stats.degree_distance_area_prunes
                  << ",\"degeneracy_prunes\":" << bound_stats.degeneracy_prunes
                  << ",\"verified\":" << (verified ? "true" : "false")
                  << ",\"compatibility_wall_time_capacity_seconds\":" << compatibility_wall_time_capacity_seconds
                  << ",\"peak_active_physical_leases\":" << peak_active_physical_leases
                  << ",\"useful_leases\":" << useful_leases
                  << ",\"empty_claim_exits\":" << empty_claim_exits
                  << ",\"cross_session_steals\":" << cross_session_steals
                  << ",\"per_threshold_useful_work\":{";
        bool first_th = true;
        for (const auto& [th, work] : per_threshold_useful_work) {
            if (!first_th) std::cout << ',';
            first_th = false;
            std::cout << "\"" << th << "\":" << work;
        }
        std::cout << "},\"per_epoch_useful_work\":{";
        bool first_ep = true;
        for (const auto& [ep, work] : per_epoch_useful_work) {
            if (!first_ep) std::cout << ',';
            first_ep = false;
            std::cout << "\"" << ep << "\":" << work;
        }
        std::cout << "}"
                  << ",\"configured_proof_regions_bound\":" << configured_proof_regions_bound
                  << ",\"resolved_proof_regions_bound\":" << resolved_proof_regions_bound
                  << ",\"peak_proof_regions\":" << peak_proof_regions
                  << ",\"suppressed_donations\":" << suppressed_donations
                  << "}\n";
    } else {
        std::cout << "status: " << (result.optimal ? "OPTIMAL" : "FEASIBLE") << '\n'
                  << "engine: " << engine << '\n'
                  << "vertices: " << graph.size() << '\n'
                  << "edges: " << graph.edge_count() << '\n'
                  << "lower bound: " << result.lower_bound << '\n'
                  << "upper bound: " << result.upper_bound << '\n'
                  << "ordering:";
        for (const auto vertex : result.ordering) std::cout << ' ' << graph.label(vertex);
        std::cout << "\nruntime seconds: " << std::fixed << std::setprecision(6) << elapsed << '\n';
        std::cout << "nodes expanded: " << result.nodes_expanded << '\n'
                  << "pruned by bound: " << result.pruned_by_bound << '\n'
                  << "pruned by dominance: " << result.pruned_by_dominance << '\n'
                  << "transposition table size: " << result.transposition_table_size << '\n';
        if (cli.engine != "legacy") {
            std::cout << "decision calls: " << v2_decision_calls << '\n'
                      << "twin-symmetric children skipped: " << v2_twin_skips << '\n'
                      << "best next bucket checks: " << best_next_bucket_checks << '\n'
                      << "best next bucket parent prunes: " << best_next_bucket_parent_prunes << '\n'
                      << "best next bucket candidates avoided: " << best_next_bucket_candidates_avoided << '\n';
        }
        if (cli.verify) std::cout << "verification: PASSED\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);
        if (options.help) {
            std::cout << usage;
            return 0;
        }
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        std::cerr << "Try --help for usage.\n";
        return 2;
    }
}
