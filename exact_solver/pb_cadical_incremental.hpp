#pragma once

#include "graph.hpp"
#include "pb_encoding.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cutwidth::pb {

enum class IncrementalStatus { sat, unsat_exploratory, timed_out, unavailable, error };

struct IncrementalResult {
    IncrementalStatus status = IncrementalStatus::unavailable;
    std::vector<std::int8_t> assignment;
    std::string diagnostic;
    double runtime_seconds = 0.0;
    std::string proof_path;
    std::vector<std::int32_t> added_unit_clauses;
};

// A learned-clause-preserving CaDiCaL session. Thresholds may only tighten.
// UNSAT is intentionally exploratory: callers must rerun the exact threshold
// through the external proof/checker path before strengthening a lower bound.
class IncrementalCadicalSession {
public:
    IncrementalCadicalSession(
        const CutwidthCnf& encoding, std::uint32_t initial_threshold,
        const std::vector<Graph::Vertex>& phase_ordering = {},
        bool trace_proof = false, bool keep_temporary_files = false);
    ~IncrementalCadicalSession();
    IncrementalCadicalSession(IncrementalCadicalSession&&) = delete;
    IncrementalCadicalSession& operator=(IncrementalCadicalSession&&) = delete;
    IncrementalCadicalSession(const IncrementalCadicalSession&) = delete;
    IncrementalCadicalSession& operator=(const IncrementalCadicalSession&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] IncrementalResult solve(
        std::uint32_t threshold, std::chrono::milliseconds time_limit);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cutwidth::pb
