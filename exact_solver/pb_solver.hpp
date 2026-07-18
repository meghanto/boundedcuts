#pragma once

#include "pb_encoding.hpp"

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace cutwidth::pb {

enum class ExternalSatStatus {
    sat,
    unsat_verified,
    unsat_unverified,
    timed_out,
    unavailable,
    invalid_output,
    process_error,
    version_mismatch,
};

struct ExternalSatOptions {
    // Absolute executable paths are required. Arguments are passed directly to
    // execv; no shell is involved. {input} and {proof} placeholders are
    // replaced in individual arguments.
    std::string solver_path;
    std::vector<std::string> solver_arguments{"{input}"};
    std::string expected_version;
    std::vector<std::string> version_arguments{"--version"};
    // Optional unit clauses streamed after the immutable base CNF. They are
    // part of the checked DIMACS instance, not unproved solver assumptions.
    std::vector<std::int32_t> unit_clauses;

    // UNSAT is accepted only when this checker is configured and exits zero.
    // Typical checker arguments are {"{input}", "{proof}"}.
    std::string proof_checker_path;
    std::vector<std::string> proof_checker_arguments{"{input}", "{proof}"};
    std::chrono::milliseconds time_limit{0};
    // Reserved inside the global limit. The SAT process is stopped this much
    // earlier so an UNSAT proof has time to be independently checked.
    std::chrono::milliseconds proof_check_reserve{0};
    bool keep_temporary_files = false;
};

struct ExternalSatResult {
    ExternalSatStatus status = ExternalSatStatus::unavailable;
    std::vector<std::int8_t> assignment; // index 0 unused; -1 unknown, 0/1 values
    std::string solver_version;
    std::string solver_output;
    std::string checker_output;
    std::string diagnostic;
    std::string temporary_directory;
    int solver_exit_code = -1;
    int checker_exit_code = -1;
    double runtime_seconds = 0.0;
    double checker_seconds = 0.0;
    bool proof_generated = false;
    bool proof_checked = false;
    std::uintmax_t proof_bytes = 0;
    std::string proof_fnv1a64;
};

struct ProofCheckResult {
    bool checked = false;
    bool timed_out = false;
    int exit_code = -1;
    double runtime_seconds = 0.0;
    std::string output;
    std::string diagnostic;
};

[[nodiscard]] ExternalSatResult solve_dimacs_external(
    const CnfFormula& formula, const ExternalSatOptions& options);

// Independently checks an already generated DRAT proof against the immutable
// base formula plus exact unit clauses added by an incremental session.
[[nodiscard]] ProofCheckResult check_drat_proof_external(
    const CnfFormula& formula, const std::vector<std::int32_t>& unit_clauses,
    const std::string& proof_path, const ExternalSatOptions& options);
[[nodiscard]] std::string file_fnv1a64(const std::string& path);

} // namespace cutwidth::pb
