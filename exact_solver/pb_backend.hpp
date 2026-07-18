#pragma once

#include "decision.hpp"
#include "pb_encoding.hpp"
#include "pb_solver.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <cstddef>

namespace cutwidth::pb {

enum class SolverKind { kissat, cadical };

struct DecisionOptions {
    CardinalityEncoding encoding = CardinalityEncoding::sequential_counter;
    SolverKind solver = SolverKind::kissat;
    ExternalSatOptions external;
    bool break_reversal_symmetry = true;
    bool channel_positions = false;
    bool split_first_vertex = false;
    bool native_incremental = false;
    std::size_t workers = 1;
};

struct DecisionProvenance {
    ExternalSatStatus external_status = ExternalSatStatus::unavailable;
    std::string solver_name;
    std::string solver_version;
    std::string encoding;
    bool position_channeling = false;
    std::string model_hash;
    std::string diagnostic;
    std::string artifact_directory;
    std::string proof_hash;
    std::uint32_t variables = 0;
    std::uint64_t clauses = 0;
    double encoding_seconds = 0.0;
    double solver_seconds = 0.0;
    double checker_seconds = 0.0;
    bool proof_generated = false;
    bool proof_checked = false;
    bool witness_verified = false;
    std::uintmax_t proof_bytes = 0;
    std::size_t branches_total = 1;
    std::size_t branches_completed = 0;
    std::size_t branches_unsat_verified = 0;
    std::size_t branches_sat_verified = 0;
    std::size_t branches_unsat_unverified = 0;
    std::size_t branches_timed_out = 0;
    std::size_t branches_other_failures = 0;
    std::size_t branches_with_proof = 0;
    bool first_vertex_split = false;
};

struct DecisionResult {
    cutwidth::DecisionResult decision;
    DecisionProvenance provenance;
};

// Exact certification boundary for the external backend. SAT is feasible only
// after independent ordering verification; UNSAT is infeasible only after the
// configured checker validates the emitted proof. Every other outcome maps to
// timed_out, which makes no mathematical claim.
[[nodiscard]] DecisionResult decide_cutwidth(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options);

[[nodiscard]] const char* status_name(ExternalSatStatus status) noexcept;
[[nodiscard]] const char* encoding_name(CardinalityEncoding encoding) noexcept;
[[nodiscard]] const char* solver_name(SolverKind solver) noexcept;

} // namespace cutwidth::pb
