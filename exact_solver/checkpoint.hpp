#pragma once

#include "decision_session.hpp"
#include "parallel_decision_session.hpp"
#include "incumbent_session.hpp"
#include "progressive_cheap_bound_session.hpp"
#include "progressive_sdp_session.hpp"
#include "residual_dp.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cutwidth {

[[nodiscard]] std::string sha256_hex(std::string_view bytes);

enum class CompletedThresholdResult { feasible, infeasible };

struct CompletedThreshold {
    std::uint32_t threshold = 0;
    CompletedThresholdResult result = CompletedThresholdResult::feasible;

    bool operator==(const CompletedThreshold&) const = default;
};

// Portable verified state only. Failed-state caches and floating-point solver
// iterates are intentionally excluded.
struct Checkpoint {
    std::string graph_hash;
    std::string solver_hash;
    std::string options_hash;
    std::vector<std::uint32_t> ordering;
    std::uint32_t lower_bound = 0;
    std::uint32_t upper_bound = 0;
    std::uint64_t elapsed_milliseconds = 0;
    std::vector<CompletedThreshold> completed_thresholds;
    bool timed_out = false;
    bool interrupted = false;

    bool operator==(const Checkpoint&) const = default;
};

// Throws std::invalid_argument when portable-state invariants do not hold.
void validate_checkpoint(const Checkpoint& checkpoint);

// Versioned, strict and deterministic encoding. The format is documented in
// checkpoint.cpp. Decoding rejects unknown, duplicate and malformed fields.
[[nodiscard]] std::string serialize_checkpoint(const Checkpoint& checkpoint);
[[nodiscard]] Checkpoint parse_checkpoint(const std::string& encoded);

// Reads and validates a checkpoint. Atomic writing uses a temporary file in
// the destination directory, flushes it, then renames it over the destination.
[[nodiscard]] Checkpoint read_checkpoint(const std::filesystem::path& path);
void write_checkpoint_atomic(const std::filesystem::path& path,
                             const Checkpoint& checkpoint);

struct SessionTelemetry {
    std::uint64_t nodes = 0;
    double busy_seconds = 0.0;
    double allocated_seconds = 0.0;
    bool has_telemetry = false;
};

struct ValueAwareEpochCheckpoint {
    std::uint32_t lower_bound = 0;
    std::uint32_t upper_bound = 0;
    std::vector<std::uint32_t> candidates;
};

struct AdaptiveCheckpoint {
    std::string graph_hash;
    std::string solver_semantic_hash;
    std::string proof_policy_hash;
    std::string candidate_order_hash;
    std::uint32_t vertex_count = 0;
    std::size_t declared_memory_bytes = 0;
    std::vector<std::uint32_t> ordering;
    std::uint32_t lower_bound = 0;
    std::uint32_t upper_bound = 0;
    std::uint64_t elapsed_milliseconds = 0;
    std::vector<CompletedThreshold> completed_thresholds;
    std::vector<SessionSnapshot> sessions;
    std::vector<ParallelDecisionSnapshot> parallel_sessions;
    std::optional<IncumbentSnapshot> incumbent;
    std::optional<sdp::ProgressiveSdpSnapshot> progressive_sdp;
    std::optional<ProgressiveCheapBoundSnapshot> progressive_cheap_bounds;
    std::optional<ResidualDpSnapshot> residual_dp;
    std::unordered_map<std::uint32_t, SessionTelemetry> session_telemetry;
    std::optional<ValueAwareEpochCheckpoint> value_aware_epoch;
};

struct CheckpointCompatibility {
    std::string graph_hash;
    std::string solver_semantic_hash;
    std::string proof_policy_hash;
    std::string candidate_order_hash;
};

void validate_adaptive_checkpoint(const AdaptiveCheckpoint& checkpoint);
void validate_checkpoint_compatibility(const AdaptiveCheckpoint& checkpoint,
                                       const CheckpointCompatibility& expected);
[[nodiscard]] AdaptiveCheckpoint read_adaptive_checkpoint(
    const std::filesystem::path& path);
void write_adaptive_checkpoint_atomic(const std::filesystem::path& path,
                                      const AdaptiveCheckpoint& checkpoint);

} // namespace cutwidth
