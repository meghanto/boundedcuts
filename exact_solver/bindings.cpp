#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "graph.hpp"
#include "optimizer_v2.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nb = nanobind;

namespace {

struct SolveOptions {
    std::size_t threads = 1;
    double time_limit = 0.0;
    std::string controller = "static";
    std::size_t memory_budget_bytes = std::size_t{16} * 1024U * 1024U * 1024U;
    std::vector<std::string> adaptive_arms{"bounds", "dfs", "alns", "sdp", "residual-dp"};
    bool verify = true;

    // Partial-state SDP is opt-in through the adaptive "sdp" arm.  When it
    // is enabled, these defaults keep an individual expensive oracle from
    // consuming an otherwise finite solve budget.  Zero retains the native
    // unlimited semantics for callers that explicitly request it.
    double sdp_total_time = 5.0;
    std::size_t sdp_max_calls = 2;
    std::size_t sdp_max_state_dimension = 64;

    // pb-sat-root fields
    std::string pb_sat_root_solver;
    std::string pb_sat_root_checker;
    std::string pb_sat_root_dir;
    double pb_sat_root_timeout = 0.0;
    std::optional<std::size_t> pb_sat_root_q;
    std::uint32_t pb_sat_root_max_gap = 2;
    std::string pb_sat_root_backend = "embedded";
    std::string pb_sat_root_ordering = "auto";
};

struct SolveResult {
    bool optimal = false;
    std::string status;
    std::uint32_t lower_bound = 0;
    std::uint32_t upper_bound = 0;
    double runtime_seconds = 0.0;
    std::uint64_t nodes_expanded = 0;
    bool verified = false;
    std::uint64_t pb_sat_root_attempts = 0;
    std::uint64_t pb_sat_root_certified_unsat = 0;
    std::uint64_t pb_sat_root_checker_successes = 0;
    std::uint32_t pb_sat_root_active_threshold = 0;
    std::uint64_t pb_sat_root_active_cardinality = 0;
    double pb_sat_root_solver_seconds = 0.0;
    double pb_sat_root_checker_seconds = 0.0;
    std::string pb_sat_root_last_result;
    std::string pb_sat_root_backend;
    std::string pb_sat_root_provenance;
    std::size_t pb_sat_root_proof_bytes = 0;
    std::string pb_sat_root_proof_hash;
    std::string pb_sat_root_ordering;
    std::uint32_t pb_sat_root_ordering_maximum_frontier = 0;
    std::uint32_t pb_sat_root_ordering_bandwidth = 0;
    std::uint64_t pb_sat_root_ordering_total_edge_span = 0;
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
    nb::object ordering;
};

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void validate(const SolveOptions& options) {
    if (options.threads == 0) throw std::invalid_argument("threads must be positive");
    if (!std::isfinite(options.time_limit) || options.time_limit < 0.0)
        throw std::invalid_argument("time_limit must be finite and non-negative");
    if (options.controller != "static" && options.controller != "adaptive")
        throw std::invalid_argument("controller must be 'static' or 'adaptive'");
    if (options.controller == "adaptive" && !contains(options.adaptive_arms, "dfs"))
        throw std::invalid_argument("adaptive_arms must contain 'dfs'");
    if (!std::isfinite(options.pb_sat_root_timeout) || options.pb_sat_root_timeout < 0.0)
        throw std::invalid_argument("pb_sat_root_timeout must be finite and non-negative");
    if (!std::isfinite(options.sdp_total_time) || options.sdp_total_time < 0.0)
        throw std::invalid_argument("sdp_total_time must be finite and non-negative");
    for (const auto& arm : options.adaptive_arms) {
        if (arm != "bounds" && arm != "dfs" && arm != "alns" && arm != "sdp" &&
            arm != "residual-dp" && arm != "pb-sat-root")
            throw std::invalid_argument("unknown adaptive arm: " + arm);
    }
    if (contains(options.adaptive_arms, "pb-sat-root")) {
        if (options.controller != "adaptive")
            throw std::invalid_argument("pb-sat-root requires the adaptive controller");
        if (options.pb_sat_root_backend != "embedded" && options.pb_sat_root_backend != "external")
            throw std::invalid_argument("pb_sat_root_backend must be 'embedded' or 'external'");
        if (options.pb_sat_root_ordering != "auto" &&
            options.pb_sat_root_ordering != "identity" &&
            options.pb_sat_root_ordering != "rcm")
            throw std::invalid_argument(
                "pb_sat_root_ordering must be 'auto', 'identity', or 'rcm'");
        if (options.pb_sat_root_backend == "external") {
            if (options.pb_sat_root_solver.empty() || options.pb_sat_root_checker.empty() ||
                options.pb_sat_root_dir.empty() || options.pb_sat_root_timeout <= 0.0)
                throw std::invalid_argument(
                    "pb-sat-root requires solver, checker, directory, and a positive timeout in external mode");
        }
    }
}

cutwidth::OptimizerV2Options native_options(const SolveOptions& input) {
    validate(input);
    cutwidth::OptimizerV2Options options;
    options.threads = input.threads;
    if (input.time_limit > 0.0) {
        constexpr double maximum = static_cast<double>(
            std::numeric_limits<std::chrono::milliseconds::rep>::max());
        const double milliseconds = std::ceil(input.time_limit * 1000.0);
        if (milliseconds > maximum) throw std::invalid_argument("time_limit is too large");
        options.time_limit = std::chrono::milliseconds(
            static_cast<std::chrono::milliseconds::rep>(milliseconds));
    }
    options.controller = input.controller == "adaptive"
        ? cutwidth::ControllerMode::adaptive
        : cutwidth::ControllerMode::static_policy;
    options.memory_budget_bytes = input.memory_budget_bytes;
    options.adaptive_arms = input.adaptive_arms;
    if (input.controller == "adaptive" && contains(input.adaptive_arms, "sdp")) {
        options.sdp_schedule = cutwidth::sdp::SdpSchedule::adaptive;
        if (input.sdp_total_time > 0.0) {
            constexpr double maximum = static_cast<double>(
                std::numeric_limits<std::chrono::milliseconds::rep>::max());
            const double milliseconds = std::ceil(input.sdp_total_time * 1000.0);
            if (milliseconds > maximum)
                throw std::invalid_argument("sdp_total_time is too large");
            options.sdp_total_time = std::chrono::milliseconds(
                static_cast<std::chrono::milliseconds::rep>(milliseconds));
        }
        options.sdp_max_calls = input.sdp_max_calls;
        options.sdp_max_state_dimension = input.sdp_max_state_dimension;
    } else {
        options.sdp_schedule = cutwidth::sdp::SdpSchedule::off;
    }

    options.pb_sat_root_solver = input.pb_sat_root_solver;
    options.pb_sat_root_checker = input.pb_sat_root_checker;
    options.pb_sat_root_dir = input.pb_sat_root_dir;
    if (input.pb_sat_root_timeout > 0.0) {
        constexpr double maximum = static_cast<double>(
            std::numeric_limits<std::chrono::milliseconds::rep>::max());
        const double milliseconds = std::ceil(input.pb_sat_root_timeout * 1000.0);
        if (milliseconds > maximum) throw std::invalid_argument("pb_sat_root_timeout is too large");
        options.pb_sat_root_timeout = std::chrono::milliseconds(
            static_cast<std::chrono::milliseconds::rep>(milliseconds));
    } else {
        options.pb_sat_root_timeout = std::chrono::milliseconds(0);
    }
    options.pb_sat_root_q = input.pb_sat_root_q;
    options.pb_sat_root_max_gap = input.pb_sat_root_max_gap;
    options.pb_sat_root_ordering = input.pb_sat_root_ordering;
    if (input.pb_sat_root_backend == "embedded") {
        options.pb_sat_root_backend = cutwidth::PbSatRootBackend::embedded;
    } else if (input.pb_sat_root_backend == "external") {
        options.pb_sat_root_backend = cutwidth::PbSatRootBackend::external;
    }

    if (options.controller == cutwidth::ControllerMode::adaptive) {
        options.use_partial_bounds = contains(options.adaptive_arms, "bounds");
        // The value-aware epoch probes U-1, U-2, ... before midpoint and
        // then uses measured work plus starvation protection to schedule the
        // persistent proof forests.  This makes the Python adaptive default
        // useful to callers who value a better layout before a stronger LB.
        options.threshold_scheduler = cutwidth::ThresholdSchedulerMode::value_aware;
        if (contains(options.adaptive_arms, "alns"))
            options.heuristic_search = cutwidth::HeuristicSearch::portfolio;
    }
    return options;
}

nb::object numpy_ordering(const std::vector<cutwidth::Graph::Vertex>& ordering) {
    auto* data = new std::uint32_t[ordering.size()];
    std::copy(ordering.begin(), ordering.end(), data);
    nb::capsule owner(data, [](void* pointer) noexcept {
        delete[] static_cast<std::uint32_t*>(pointer);
    });
    const std::size_t shape[] = {ordering.size()};
    nb::ndarray<nb::numpy, std::uint32_t, nb::ndim<1>, nb::c_contig,
                nb::device::cpu> array(data, 1, shape, owner);
    return nb::cast(array);
}

SolveResult solve(const cutwidth::Graph& graph, const SolveOptions& input) {
    if (input.pb_sat_root_q && *input.pb_sat_root_q > graph.size())
        throw std::invalid_argument("pb_sat_root_q exceeds the graph vertex count");
    auto options = native_options(input);
    const auto started = std::chrono::steady_clock::now();
    cutwidth::OptimizerV2Result raw;
    {
        nb::gil_scoped_release release;
        raw = cutwidth::optimize_cutwidth_v2(graph, std::move(options));
    }

    SolveResult result;
    result.optimal = raw.optimal;
    result.status = raw.optimal ? "OPTIMAL" : "FEASIBLE";
    result.lower_bound = raw.lower_bound;
    result.upper_bound = raw.upper_bound;
    result.runtime_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    result.nodes_expanded = raw.stats.nodes_expanded;
    result.pb_sat_root_attempts = raw.stats.pb_sat_root_attempts;
    result.pb_sat_root_certified_unsat = raw.stats.pb_sat_root_certified_unsat;
    result.pb_sat_root_checker_successes = raw.stats.pb_sat_root_checker_successes;
    result.pb_sat_root_active_threshold = raw.stats.pb_sat_root_active_threshold;
    result.pb_sat_root_active_cardinality = raw.stats.pb_sat_root_active_cardinality;
    result.pb_sat_root_solver_seconds = raw.stats.pb_sat_root_solver_seconds;
    result.pb_sat_root_checker_seconds = raw.stats.pb_sat_root_checker_seconds;
    result.pb_sat_root_last_result = raw.stats.pb_sat_root_last_result;
    result.pb_sat_root_backend = raw.stats.pb_sat_root_backend;
    result.pb_sat_root_provenance = raw.stats.pb_sat_root_provenance;
    result.pb_sat_root_proof_bytes = raw.stats.pb_sat_root_proof_bytes;
    result.pb_sat_root_proof_hash = raw.stats.pb_sat_root_proof_hash;
    result.pb_sat_root_ordering = raw.stats.pb_sat_root_ordering;
    result.pb_sat_root_ordering_maximum_frontier = raw.stats.pb_sat_root_ordering_maximum_frontier;
    result.pb_sat_root_ordering_bandwidth = raw.stats.pb_sat_root_ordering_bandwidth;
    result.pb_sat_root_ordering_total_edge_span = raw.stats.pb_sat_root_ordering_total_edge_span;
    result.sdp_attempted = raw.stats.sdp_attempted;
    result.sdp_available = raw.stats.sdp_available;
    result.sdp_raw_converged = raw.stats.sdp_raw_converged;
    result.sdp_primal_residual = raw.stats.sdp_primal_residual;
    result.sdp_certified_lower_bound = raw.stats.sdp_certified_lower_bound;
    result.sdp_primal_objective = raw.stats.sdp_primal_objective;
    result.sdp_dual_objective = raw.stats.sdp_dual_objective;
    result.sdp_dual_residual = raw.stats.sdp_dual_residual;
    result.sdp_solve_seconds = raw.stats.sdp_solve_seconds;
    result.sdp_solver_iterations = raw.stats.sdp_solver_iterations;
    result.sdp_solver_status = raw.stats.sdp_solver_status;
    result.sdp_bisection_calls = raw.stats.sdp_bisection_calls;
    result.sdp_triangle_cuts = raw.stats.sdp_triangle_cuts;
    result.sdp_state_requests = raw.stats.sdp_state_requests;
    result.sdp_state_certified = raw.stats.sdp_state_certified;
    result.sdp_state_prunes = raw.stats.sdp_state_prunes;
    result.sdp_state_cache_hits = raw.stats.sdp_state_cache_hits;
    result.sdp_state_calls = raw.stats.sdp_state_calls;
    result.sdp_state_busy = raw.stats.sdp_state_busy;
    result.sdp_state_budget_rejections = raw.stats.sdp_state_budget_rejections;
    result.sdp_state_uncertified = raw.stats.sdp_state_uncertified;
    result.sdp_state_dimension_rejections = raw.stats.sdp_state_dimension_rejections;
    result.sdp_state_preferred_max_dimension = raw.stats.sdp_state_preferred_max_dimension;
#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
    // Backend availability is a build/runtime capability, independent of
    // whether this particular graph admitted an SDP job before it was solved.
    result.sdp_available = true;
#endif
    if (input.verify) {
        if (!graph.validate_ordering(raw.ordering) ||
            graph.ordering_cutwidth(raw.ordering) != raw.upper_bound)
            throw std::runtime_error("solver returned an invalid ordering or upper bound");
        result.verified = true;
    }
    result.ordering = numpy_ordering(raw.ordering);
    return result;
}

} // namespace

NB_MODULE(_boundedcuts, module) {
    module.doc() = "Native bindings for the BoundedCuts exact cutwidth solver";

    nb::class_<cutwidth::Graph>(module, "Graph")
        .def("__init__", [](cutwidth::Graph* self, std::size_t vertex_count,
                            nb::ndarray<nb::numpy, const std::uint32_t,
                                        nb::shape<-1, 2>, nb::c_contig,
                                        nb::device::cpu> edges,
                            std::vector<std::string> labels) {
            const auto endpoint_count = edges.shape(0) * std::size_t{2};
            new (self) cutwidth::Graph(cutwidth::Graph::from_interleaved_edges(
                vertex_count,
                std::span<const std::uint32_t>(edges.data(), endpoint_count),
                std::move(labels)));
        }, nb::arg("vertex_count"), nb::arg("edges").noconvert(),
           nb::arg("labels") = std::vector<std::string>{})
        .def_prop_ro("vertex_count", &cutwidth::Graph::size)
        .def_prop_ro("edge_count", &cutwidth::Graph::edge_count)
        .def("validate_ordering", &cutwidth::Graph::validate_ordering)
        .def("ordering_cutwidth", &cutwidth::Graph::ordering_cutwidth);

    nb::class_<SolveOptions>(module, "SolveOptions")
        .def(nb::init<>())
        .def_rw("threads", &SolveOptions::threads)
        .def_rw("time_limit", &SolveOptions::time_limit)
        .def_rw("controller", &SolveOptions::controller)
        .def_rw("memory_budget_bytes", &SolveOptions::memory_budget_bytes)
        .def_rw("adaptive_arms", &SolveOptions::adaptive_arms)
        .def_rw("verify", &SolveOptions::verify)
        .def_rw("sdp_total_time", &SolveOptions::sdp_total_time)
        .def_rw("sdp_max_calls", &SolveOptions::sdp_max_calls)
        .def_rw("sdp_max_state_dimension", &SolveOptions::sdp_max_state_dimension)
        .def_rw("pb_sat_root_solver", &SolveOptions::pb_sat_root_solver)
        .def_rw("pb_sat_root_checker", &SolveOptions::pb_sat_root_checker)
        .def_rw("pb_sat_root_dir", &SolveOptions::pb_sat_root_dir)
        .def_rw("pb_sat_root_timeout", &SolveOptions::pb_sat_root_timeout)
        .def_rw("pb_sat_root_q", &SolveOptions::pb_sat_root_q)
        .def_rw("pb_sat_root_max_gap", &SolveOptions::pb_sat_root_max_gap)
        .def_rw("pb_sat_root_backend", &SolveOptions::pb_sat_root_backend)
        .def_rw("pb_sat_root_ordering", &SolveOptions::pb_sat_root_ordering);

    nb::class_<SolveResult>(module, "SolveResult")
        .def_ro("optimal", &SolveResult::optimal)
        .def_ro("status", &SolveResult::status)
        .def_ro("lower_bound", &SolveResult::lower_bound)
        .def_ro("upper_bound", &SolveResult::upper_bound)
        .def_ro("runtime_seconds", &SolveResult::runtime_seconds)
        .def_ro("nodes_expanded", &SolveResult::nodes_expanded)
        .def_ro("verified", &SolveResult::verified)
        .def_ro("pb_sat_root_attempts", &SolveResult::pb_sat_root_attempts)
        .def_ro("pb_sat_root_certified_unsat", &SolveResult::pb_sat_root_certified_unsat)
        .def_ro("pb_sat_root_checker_successes", &SolveResult::pb_sat_root_checker_successes)
        .def_ro("pb_sat_root_active_threshold", &SolveResult::pb_sat_root_active_threshold)
        .def_ro("pb_sat_root_active_cardinality", &SolveResult::pb_sat_root_active_cardinality)
        .def_ro("pb_sat_root_solver_seconds", &SolveResult::pb_sat_root_solver_seconds)
        .def_ro("pb_sat_root_checker_seconds", &SolveResult::pb_sat_root_checker_seconds)
        .def_ro("pb_sat_root_last_result", &SolveResult::pb_sat_root_last_result)
        .def_ro("pb_sat_root_backend", &SolveResult::pb_sat_root_backend)
        .def_ro("pb_sat_root_provenance", &SolveResult::pb_sat_root_provenance)
        .def_ro("pb_sat_root_proof_bytes", &SolveResult::pb_sat_root_proof_bytes)
        .def_ro("pb_sat_root_proof_hash", &SolveResult::pb_sat_root_proof_hash)
        .def_ro("pb_sat_root_ordering", &SolveResult::pb_sat_root_ordering)
        .def_ro("pb_sat_root_ordering_maximum_frontier", &SolveResult::pb_sat_root_ordering_maximum_frontier)
        .def_ro("pb_sat_root_ordering_bandwidth", &SolveResult::pb_sat_root_ordering_bandwidth)
        .def_ro("pb_sat_root_ordering_total_edge_span", &SolveResult::pb_sat_root_ordering_total_edge_span)
        .def_ro("sdp_attempted", &SolveResult::sdp_attempted)
        .def_ro("sdp_available", &SolveResult::sdp_available)
        .def_ro("sdp_raw_converged", &SolveResult::sdp_raw_converged)
        .def_ro("sdp_primal_residual", &SolveResult::sdp_primal_residual)
        .def_ro("sdp_certified_lower_bound", &SolveResult::sdp_certified_lower_bound)
        .def_ro("sdp_primal_objective", &SolveResult::sdp_primal_objective)
        .def_ro("sdp_dual_objective", &SolveResult::sdp_dual_objective)
        .def_ro("sdp_dual_residual", &SolveResult::sdp_dual_residual)
        .def_ro("sdp_solve_seconds", &SolveResult::sdp_solve_seconds)
        .def_ro("sdp_solver_iterations", &SolveResult::sdp_solver_iterations)
        .def_ro("sdp_solver_status", &SolveResult::sdp_solver_status)
        .def_ro("sdp_bisection_calls", &SolveResult::sdp_bisection_calls)
        .def_ro("sdp_triangle_cuts", &SolveResult::sdp_triangle_cuts)
        .def_ro("sdp_state_requests", &SolveResult::sdp_state_requests)
        .def_ro("sdp_state_certified", &SolveResult::sdp_state_certified)
        .def_ro("sdp_state_prunes", &SolveResult::sdp_state_prunes)
        .def_ro("sdp_state_cache_hits", &SolveResult::sdp_state_cache_hits)
        .def_ro("sdp_state_calls", &SolveResult::sdp_state_calls)
        .def_ro("sdp_state_busy", &SolveResult::sdp_state_busy)
        .def_ro("sdp_state_budget_rejections", &SolveResult::sdp_state_budget_rejections)
        .def_ro("sdp_state_uncertified", &SolveResult::sdp_state_uncertified)
        .def_ro("sdp_state_dimension_rejections", &SolveResult::sdp_state_dimension_rejections)
        .def_ro("sdp_state_preferred_max_dimension", &SolveResult::sdp_state_preferred_max_dimension)
        .def_ro("ordering", &SolveResult::ordering);

    module.def("solve", &solve, nb::arg("graph"), nb::arg("options"));
    module.def("capabilities", [] {
        nb::dict result;
        result["dfs"] = true;
        result["partial_bounds"] = true;
        result["residual_dp"] = true;
        result["lagrangian_bounds"] = true;
        result["sdp_formulation"] = true;
        result["sdp_certificate_verifier"] = true;
        result["pb_sat_root"] = true;
        result["in_memory_drat_checker"] = true;
#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
        result["clarabel"] = true;
#else
        result["clarabel"] = false;
#endif
#ifdef CUTWIDTH_HAVE_CADICAL
        result["cadical"] = true;
        result["embedded_pb_sat_root"] = true;
#else
        result["cadical"] = false;
        result["embedded_pb_sat_root"] = false;
#endif
#ifdef CUTWIDTH_HAVE_HIGHS
        result["highs"] = true;
#else
        result["highs"] = false;
#endif
        return result;
    });
}
