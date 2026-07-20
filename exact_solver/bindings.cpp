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

    // pb-sat-root fields
    std::string pb_sat_root_solver;
    std::string pb_sat_root_checker;
    std::string pb_sat_root_dir;
    double pb_sat_root_timeout = 0.0;
    std::optional<std::size_t> pb_sat_root_q;
    std::uint32_t pb_sat_root_max_gap = 2;
    std::string pb_sat_root_backend = "embedded";
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
    options.sdp_schedule = cutwidth::sdp::SdpSchedule::off;

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
    if (input.pb_sat_root_backend == "embedded") {
        options.pb_sat_root_backend = cutwidth::PbSatRootBackend::embedded;
    } else if (input.pb_sat_root_backend == "external") {
        options.pb_sat_root_backend = cutwidth::PbSatRootBackend::external;
    }

    if (options.controller == cutwidth::ControllerMode::adaptive) {
        options.use_partial_bounds = contains(options.adaptive_arms, "bounds");
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
        .def_rw("pb_sat_root_solver", &SolveOptions::pb_sat_root_solver)
        .def_rw("pb_sat_root_checker", &SolveOptions::pb_sat_root_checker)
        .def_rw("pb_sat_root_dir", &SolveOptions::pb_sat_root_dir)
        .def_rw("pb_sat_root_timeout", &SolveOptions::pb_sat_root_timeout)
        .def_rw("pb_sat_root_q", &SolveOptions::pb_sat_root_q)
        .def_rw("pb_sat_root_max_gap", &SolveOptions::pb_sat_root_max_gap)
        .def_rw("pb_sat_root_backend", &SolveOptions::pb_sat_root_backend);

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
