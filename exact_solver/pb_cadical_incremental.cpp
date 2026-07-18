#include "pb_cadical_incremental.hpp"

#include <chrono>
#include <limits>
#include <filesystem>
#include <utility>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#ifdef CUTWIDTH_HAVE_CADICAL
#include <cadical.hpp>
#endif

namespace cutwidth::pb {
namespace {
using Clock = std::chrono::steady_clock;
}

struct IncrementalCadicalSession::Impl {
    std::uint32_t current_threshold = 0;
    std::uint32_t variable_count = 0;
    std::vector<std::vector<std::uint32_t>> prefix_variables;
    std::vector<std::vector<std::uint32_t>> crossing_outputs;
    std::string diagnostic;
    std::string temporary_directory;
    std::string proof_path;
    std::vector<std::int32_t> added_units;
    bool proof_open = false;
    bool keep_temporary_files = false;
#ifdef CUTWIDTH_HAVE_CADICAL
    struct DeadlineTerminator final : CaDiCaL::Terminator {
        Clock::time_point deadline = Clock::time_point::max();
        bool terminate() override { return Clock::now() >= deadline; }
    } terminator;
    CaDiCaL::Solver solver;
#endif
};

IncrementalCadicalSession::IncrementalCadicalSession(
    const CutwidthCnf& encoding, std::uint32_t initial_threshold,
    const std::vector<Graph::Vertex>& phase_ordering,
    bool trace_proof, bool keep_temporary_files)
    : impl_(std::make_unique<Impl>()) {
    impl_->current_threshold = initial_threshold;
    impl_->variable_count = encoding.formula.variable_count;
    impl_->prefix_variables = encoding.prefix_variables;
    impl_->crossing_outputs = encoding.crossing_count_outputs;
    impl_->keep_temporary_files = keep_temporary_files;
#ifdef CUTWIDTH_HAVE_CADICAL
    impl_->solver.set("quiet", 1);
    if (trace_proof) {
#if defined(__unix__) || defined(__APPLE__)
        char directory_template[] = "/tmp/cutwidth-pb-inc-XXXXXX";
        if (char* directory = ::mkdtemp(directory_template)) {
            impl_->temporary_directory = directory;
            impl_->proof_path = (std::filesystem::path(directory) / "proof.drat").string();
            impl_->proof_open = impl_->solver.trace_proof(impl_->proof_path.c_str());
        }
#endif
        if (!impl_->proof_open)
            impl_->diagnostic = "could not start incremental DRAT trace";
    }
    for (const auto& clause : encoding.formula.clauses) {
        for (const auto literal : clause) impl_->solver.add(literal);
        impl_->solver.add(0);
    }
    if (phase_ordering.size() == encoding.vertex_count) {
        std::vector<std::size_t> position(encoding.vertex_count);
        bool valid = true;
        for (std::size_t index = 0; index < phase_ordering.size(); ++index) {
            if (phase_ordering[index] >= position.size()) { valid = false; break; }
            position[phase_ordering[index]] = index;
        }
        if (valid) {
            for (std::size_t vertex = 0; vertex < encoding.vertex_count; ++vertex)
                for (std::size_t prefix = 0;
                     prefix < encoding.prefix_variables[vertex].size(); ++prefix) {
                    const auto variable = static_cast<int>(
                        encoding.prefix_variables[vertex][prefix]);
                    impl_->solver.phase(position[vertex] <= prefix ? variable : -variable);
                }
        }
    }
#else
    (void)phase_ordering;
    (void)trace_proof;
    impl_->diagnostic = "pinned native CaDiCaL support was not compiled";
#endif
}

IncrementalCadicalSession::~IncrementalCadicalSession() {
#ifdef CUTWIDTH_HAVE_CADICAL
    if (impl_ && impl_->proof_open) {
        impl_->solver.close_proof_trace(false);
        impl_->proof_open = false;
    }
#endif
    if (impl_ && !impl_->keep_temporary_files && !impl_->temporary_directory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(impl_->temporary_directory, ignored);
    }
}
bool IncrementalCadicalSession::available() const noexcept {
#ifdef CUTWIDTH_HAVE_CADICAL
    return true;
#else
    return false;
#endif
}

IncrementalResult IncrementalCadicalSession::solve(
    std::uint32_t threshold, std::chrono::milliseconds time_limit) {
    IncrementalResult result;
#ifndef CUTWIDTH_HAVE_CADICAL
    (void)threshold;
    (void)time_limit;
    result.diagnostic = impl_->diagnostic;
    return result;
#else
    if (threshold > impl_->current_threshold) {
        result.status = IncrementalStatus::error;
        result.diagnostic = "incremental CaDiCaL threshold cannot be loosened";
        return result;
    }
    if (threshold < impl_->current_threshold) {
        for (const auto& outputs : impl_->crossing_outputs) {
            if (threshold >= outputs.size()) {
                result.status = IncrementalStatus::error;
                result.diagnostic = "incremental crossing counter lacks requested threshold";
                return result;
            }
            impl_->solver.add(-static_cast<int>(outputs[threshold]));
            impl_->solver.add(0);
            impl_->added_units.push_back(-static_cast<std::int32_t>(outputs[threshold]));
        }
        impl_->current_threshold = threshold;
    }
    const auto started = Clock::now();
    impl_->terminator.deadline = time_limit.count() == 0
        ? Clock::time_point::max() : started + time_limit;
    impl_->solver.connect_terminator(&impl_->terminator);
    const int status = impl_->solver.solve();
    impl_->solver.disconnect_terminator();
    result.runtime_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    if (status == 10) {
        result.status = IncrementalStatus::sat;
        result.assignment.assign(impl_->variable_count + 1, -1);
        for (const auto& row : impl_->prefix_variables)
            for (const auto variable : row)
                result.assignment[variable] = impl_->solver.val(static_cast<int>(variable)) > 0;
    } else if (status == 20) {
        result.status = IncrementalStatus::unsat_exploratory;
        if (impl_->proof_open) {
            impl_->solver.conclude();
            impl_->solver.close_proof_trace(false);
            impl_->proof_open = false;
            result.proof_path = impl_->proof_path;
            result.added_unit_clauses = impl_->added_units;
        }
    } else {
        result.status = IncrementalStatus::timed_out;
    }
    return result;
#endif
}

} // namespace cutwidth::pb
