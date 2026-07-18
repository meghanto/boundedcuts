#pragma once

#include "graph.hpp"
#include "memory_governor.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace cutwidth {

struct ResidualDpProjection {
    std::size_t residual_vertices = 0;
    std::size_t states = 0;
    std::size_t table_bytes = 0;
    std::size_t workspace_bytes = 0;
    std::size_t peak_bytes = 0;
};

[[nodiscard]] std::optional<ResidualDpProjection> project_residual_dp(
    std::size_t residual_vertices, std::size_t graph_word_count) noexcept;

struct ResidualDpEvent {
    bool applicable = false;
    bool complete = false;
    std::optional<std::uint32_t> exact_completion;
    std::uint64_t states_completed = 0;
    std::size_t peak_bytes = 0;
};

// Value-only checkpoint seam for a persistent residual DP.  It intentionally
// excludes the governor lease: restore reacquires that lease against the
// current governor before admitting any restored table memory.
struct ResidualDpSnapshot {
    std::vector<Graph::Mask> initial_prefix;
    std::vector<Graph::Vertex> remaining;
    std::optional<ResidualDpProjection> projection;
    std::vector<std::uint32_t> table;
    std::size_t next_state = 1;
    bool applicable = false;
    bool complete = false;
};

// Persistent exact completion DP for one canonical prefix. Applicability is
// decided exclusively by overflow-safe projected bytes and a governor lease.
class ResidualDpSession {
public:
    ResidualDpSession(const Graph& graph, std::span<const Graph::Mask> prefix,
                      std::shared_ptr<MemoryGovernor> governor);
    [[nodiscard]] ResidualDpEvent service(
        std::uint64_t work_units,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max());
    [[nodiscard]] bool applicable() const noexcept { return applicable_; }
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] const std::optional<ResidualDpProjection>& projection() const noexcept {
        return projection_;
    }
    [[nodiscard]] const std::vector<Graph::Vertex>& remaining() const noexcept {
        return remaining_;
    }
    // Reconstruct the exact witness ordering of the remaining vertices in
    // forward placement order.  Validates the recurrence at every step and
    // throws std::logic_error if the table is incomplete.
    [[nodiscard]] std::vector<Graph::Vertex> reconstruct_witness() const;
    [[nodiscard]] ResidualDpSnapshot snapshot() const;
    [[nodiscard]] static ResidualDpSession restore(
        const Graph& graph, const ResidualDpSnapshot& snapshot,
        std::shared_ptr<MemoryGovernor> governor);

private:
    const Graph& graph_;
    std::vector<Graph::Mask> initial_prefix_;
    std::vector<Graph::Vertex> remaining_;
    std::shared_ptr<MemoryGovernor> governor_;
    std::optional<MemoryGovernor::Lease> lease_;
    std::optional<ResidualDpProjection> projection_;
    std::vector<std::uint32_t> table_;
    std::size_t next_state_ = 1;
    bool applicable_ = false;
    bool complete_ = false;
};

} // namespace cutwidth
