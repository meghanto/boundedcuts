#include "pb_backend.hpp"
#include "pb_cadical_incremental.hpp"
#include "pb_drat_trim_adapter.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace cutwidth::pb {
namespace {
using Clock = std::chrono::steady_clock;
}

const char* status_name(ExternalSatStatus status) noexcept {
    switch (status) {
    case ExternalSatStatus::sat: return "SAT";
    case ExternalSatStatus::unsat_verified: return "UNSAT_VERIFIED";
    case ExternalSatStatus::unsat_unverified: return "UNSAT_UNVERIFIED";
    case ExternalSatStatus::timed_out: return "TIMEOUT";
    case ExternalSatStatus::unavailable: return "UNAVAILABLE";
    case ExternalSatStatus::invalid_output: return "INVALID_OUTPUT";
    case ExternalSatStatus::process_error: return "PROCESS_ERROR";
    case ExternalSatStatus::version_mismatch: return "VERSION_MISMATCH";
    }
    return "UNKNOWN";
}

const char* encoding_name(CardinalityEncoding encoding) noexcept {
    return encoding == CardinalityEncoding::sequential_counter
        ? "sequential-counter" : "totalizer";
}

const char* solver_name(SolverKind solver) noexcept {
    return solver == SolverKind::kissat ? "kissat" : "cadical";
}

DecisionResult decide_cutwidth(const Graph& graph, std::uint32_t threshold,
                               DecisionOptions options) {
    DecisionResult result;
    result.decision.threshold = threshold;
    result.decision.status = cutwidth::DecisionStatus::timed_out;
    result.provenance.solver_name = solver_name(options.solver);
    result.provenance.encoding = encoding_name(options.encoding);
    result.provenance.position_channeling = options.channel_positions;
    result.provenance.first_vertex_split = options.split_first_vertex;

    const auto encode_started = Clock::now();
    CutwidthCnf encoded;
    try {
        encoded = encode_cutwidth_threshold(
            graph, threshold, {options.encoding, options.break_reversal_symmetry,
                               options.channel_positions});
    } catch (const std::exception& error) {
        result.provenance.diagnostic = std::string("CNF encoding failed: ") + error.what();
        return result;
    }
    result.provenance.encoding_seconds =
        std::chrono::duration<double>(Clock::now() - encode_started).count();
    result.provenance.model_hash = encoded.metadata.dimacs_fnv1a64;
    result.provenance.variables = encoded.metadata.variables;
    result.provenance.clauses = encoded.metadata.clauses;

    if (options.external.time_limit.count() != 0) {
        const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - encode_started);
        if (used >= options.external.time_limit) {
            result.provenance.external_status = ExternalSatStatus::timed_out;
            result.provenance.diagnostic = "global PB deadline expired during CNF encoding";
            return result;
        }
        options.external.time_limit -= used;
    }
    if (options.native_incremental) {
        if (options.solver != SolverKind::cadical) {
            result.provenance.diagnostic =
                "native incremental mode requires --pb-solver cadical";
            return result;
        }
        IncrementalCadicalSession session(
            encoded, threshold, {}, true, options.external.keep_temporary_files);
        if (!session.available()) {
            result.provenance.external_status = ExternalSatStatus::unavailable;
            result.provenance.diagnostic =
                "pinned native CaDiCaL support was not compiled";
            return result;
        }
        auto solve_budget = options.external.time_limit;
        if (solve_budget.count() != 0 &&
            options.external.proof_check_reserve < solve_budget)
            solve_budget -= options.external.proof_check_reserve;
        const auto native = session.solve(threshold, solve_budget);
        result.provenance.solver_name = "cadical-incremental";
        result.provenance.solver_version = "2.1.3";
        result.provenance.solver_seconds = native.runtime_seconds;
        result.provenance.branches_completed = 1;
        if (native.status == IncrementalStatus::sat) {
            try {
                auto ordering = decode_ordering(encoded, native.assignment);
                if (!verify_ordering(graph, ordering, threshold)) {
                    result.provenance.external_status = ExternalSatStatus::invalid_output;
                    result.provenance.diagnostic =
                        "native SAT witness rejected by independent verification";
                    return result;
                }
                result.provenance.external_status = ExternalSatStatus::sat;
                result.provenance.witness_verified = true;
                result.provenance.branches_sat_verified = 1;
                result.decision.status = cutwidth::DecisionStatus::feasible;
                result.decision.ordering = std::move(ordering);
            } catch (const std::exception& error) {
                result.provenance.external_status = ExternalSatStatus::invalid_output;
                result.provenance.diagnostic =
                    std::string("native SAT witness decoding failed: ") + error.what();
            }
            return result;
        }
        if (native.status == IncrementalStatus::unsat_exploratory) {
            bool has_memory_proof = !native.proof_bytes.empty();
            result.provenance.proof_generated = has_memory_proof || !native.proof_path.empty();
            if (has_memory_proof) {
                result.provenance.proof_bytes = native.proof_bytes.size();
                result.provenance.proof_hash = native.proof_hash;
            } else if (!native.proof_path.empty()) {
                std::error_code error;
                result.provenance.proof_bytes =
                    std::filesystem::file_size(native.proof_path, error);
                if (error) result.provenance.proof_bytes = 0;
                else result.provenance.proof_hash = file_fnv1a64(native.proof_path);
            }
            auto checker_options = options.external;
            if (checker_options.time_limit.count() != 0) {
                const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - encode_started);
                checker_options.time_limit = used >= checker_options.time_limit
                    ? std::chrono::milliseconds{1} : checker_options.time_limit - used;
            }
            if (has_memory_proof) {
                DratCheckerResult checked = verify_proof_in_memory(
                    encoded.formula, native.added_unit_clauses,
                    native.proof_bytes, checker_options.time_limit
                );
                result.provenance.checker_seconds = checked.runtime_seconds;
                result.provenance.proof_checked = (checked.status == DratCheckerStatus::verified);
                if (checked.status == DratCheckerStatus::verified) {
                    result.provenance.external_status = ExternalSatStatus::unsat_verified;
                    result.provenance.branches_unsat_verified = 1;
                    result.decision.status = cutwidth::DecisionStatus::infeasible;
                } else {
                    result.provenance.external_status = ExternalSatStatus::unsat_unverified;
                    result.provenance.branches_unsat_unverified = 1;
                    if (checked.status == DratCheckerStatus::timeout) {
                        result.provenance.diagnostic = "in-memory checker timed out";
                    } else if (checked.status == DratCheckerStatus::error) {
                        result.provenance.diagnostic = "in-memory checker error";
                    } else {
                        result.provenance.diagnostic = "in-memory checker not verified";
                    }
                }
            } else {
                const auto checked = check_drat_proof_external(
                    encoded.formula, native.added_unit_clauses,
                    native.proof_path, checker_options);
                result.provenance.checker_seconds = checked.runtime_seconds;
                result.provenance.proof_checked = checked.checked;
                if (checked.checked) {
                    result.provenance.external_status = ExternalSatStatus::unsat_verified;
                    result.provenance.branches_unsat_verified = 1;
                    result.decision.status = cutwidth::DecisionStatus::infeasible;
                } else {
                    result.provenance.external_status = ExternalSatStatus::unsat_unverified;
                    result.provenance.branches_unsat_unverified = 1;
                    result.provenance.diagnostic = checked.diagnostic;
                }
            }
            return result;
        }
        result.provenance.external_status = native.status == IncrementalStatus::timed_out
            ? ExternalSatStatus::timed_out : ExternalSatStatus::process_error;
        result.provenance.diagnostic = native.diagnostic;
        if (native.status == IncrementalStatus::timed_out)
            result.provenance.branches_timed_out = 1;
        else
            result.provenance.branches_other_failures = 1;
        return result;
    }
    if (options.split_first_vertex && graph.size() > 1) {
        const std::size_t branch_count = graph.size();
        result.provenance.branches_total = branch_count;
        const auto portfolio_started = Clock::now();
        const auto portfolio_limit = options.external.time_limit;
        std::atomic<std::size_t> next_branch{0};
        std::atomic<bool> witness_found{false};
        std::vector<ExternalSatResult> branch_results(branch_count);
        std::vector<std::uint8_t> completed(branch_count, 0);
        std::vector<Graph::Vertex> verified_ordering;
        std::mutex witness_mutex;

        auto worker = [&] {
            while (!witness_found.load(std::memory_order_relaxed)) {
                const auto branch = next_branch.fetch_add(1, std::memory_order_relaxed);
                if (branch >= branch_count) return;
                auto external_options = options.external;
                if (portfolio_limit.count() != 0) {
                    const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - portfolio_started);
                    if (used >= portfolio_limit) return;
                    external_options.time_limit = portfolio_limit - used;
                }
                external_options.unit_clauses.reserve(graph.size());
                for (std::size_t vertex = 0; vertex < graph.size(); ++vertex) {
                    const auto variable = encoded.prefix_variables[vertex][0];
                    const auto literal = static_cast<std::int32_t>(variable);
                    external_options.unit_clauses.push_back(
                        vertex == branch ? literal : -literal);
                }
                auto external = solve_dimacs_external(
                    encoded.formula, external_options);
                branch_results[branch] = std::move(external);
                completed[branch] = 1;
                if (branch_results[branch].status != ExternalSatStatus::sat) continue;
                try {
                    auto ordering = decode_ordering(encoded, branch_results[branch].assignment);
                    if (!verify_ordering(graph, ordering, threshold)) continue;
                    {
                        std::lock_guard lock(witness_mutex);
                        if (verified_ordering.empty()) verified_ordering = std::move(ordering);
                    }
                    witness_found.store(true, std::memory_order_relaxed);
                } catch (const std::exception&) {
                    // A malformed SAT model is nonconclusive and cannot stop
                    // the remaining exact partition.
                }
            }
        };

        const auto workers = std::max<std::size_t>(1,
            std::min({options.workers, branch_count, std::size_t{64}}));
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) threads.emplace_back(worker);
        for (auto& thread : threads) thread.join();

        bool all_unsat = true;
        std::uint64_t proof_set_hash = 14695981039346656037ULL;
        for (std::size_t branch = 0; branch < branch_count; ++branch) {
            if (!completed[branch]) { all_unsat = false; continue; }
            ++result.provenance.branches_completed;
            const auto& external = branch_results[branch];
            if (result.provenance.solver_version.empty())
                result.provenance.solver_version = external.solver_version;
            result.provenance.solver_seconds += external.runtime_seconds;
            result.provenance.checker_seconds += external.checker_seconds;
            result.provenance.proof_bytes += external.proof_bytes;
            if (external.proof_generated) ++result.provenance.branches_with_proof;
            if (external.status == ExternalSatStatus::unsat_verified &&
                external.proof_checked) {
                ++result.provenance.branches_unsat_verified;
                for (const unsigned char byte : external.proof_fnv1a64) {
                    proof_set_hash ^= byte;
                    proof_set_hash *= 1099511628211ULL;
                }
            } else {
                all_unsat = false;
                if (external.status == ExternalSatStatus::unsat_unverified)
                    ++result.provenance.branches_unsat_unverified;
                else if (external.status == ExternalSatStatus::timed_out)
                    ++result.provenance.branches_timed_out;
                else
                    ++result.provenance.branches_other_failures;
            }
        }
        if (!verified_ordering.empty()) {
            result.provenance.external_status = ExternalSatStatus::sat;
            result.provenance.witness_verified = true;
            result.provenance.branches_sat_verified = 1;
            result.decision.status = cutwidth::DecisionStatus::feasible;
            result.decision.ordering = std::move(verified_ordering);
            return result;
        }
        if (all_unsat && result.provenance.branches_completed == branch_count) {
            result.provenance.external_status = ExternalSatStatus::unsat_verified;
            result.provenance.proof_generated = true;
            result.provenance.proof_checked = true;
            result.provenance.proof_hash = "fnv1a64-set:" + std::to_string(proof_set_hash);
            result.decision.status = cutwidth::DecisionStatus::infeasible;
            return result;
        }
        result.provenance.external_status = ExternalSatStatus::timed_out;
        result.provenance.diagnostic =
            "first-vertex partition did not certify every branch";
        return result;
    }

    const auto external = solve_dimacs_external(encoded.formula, options.external);
    result.provenance.branches_completed = 1;
    result.provenance.external_status = external.status;
    result.provenance.solver_version = external.solver_version;
    result.provenance.diagnostic = external.diagnostic;
    if (options.external.keep_temporary_files)
        result.provenance.artifact_directory = external.temporary_directory;
    result.provenance.proof_hash = external.proof_fnv1a64;
    result.provenance.solver_seconds = external.runtime_seconds;
    result.provenance.checker_seconds = external.checker_seconds;
    result.provenance.proof_generated = external.proof_generated;
    result.provenance.proof_checked = external.proof_checked;
    result.provenance.proof_bytes = external.proof_bytes;

    if (external.status == ExternalSatStatus::unsat_verified && external.proof_checked) {
        result.provenance.branches_unsat_verified = 1;
        result.decision.status = cutwidth::DecisionStatus::infeasible;
        return result;
    }
    if (external.status != ExternalSatStatus::sat) return result;
    try {
        auto ordering = decode_ordering(encoded, external.assignment);
        if (!verify_ordering(graph, ordering, threshold)) {
            result.provenance.diagnostic =
                "SAT witness rejected by independent cutwidth verification";
            return result;
        }
        result.provenance.witness_verified = true;
        result.provenance.branches_sat_verified = 1;
        result.decision.status = cutwidth::DecisionStatus::feasible;
        result.decision.ordering = std::move(ordering);
    } catch (const std::exception& error) {
        result.provenance.diagnostic = std::string("SAT witness decoding failed: ") + error.what();
    }
    return result;
}

} // namespace cutwidth::pb
