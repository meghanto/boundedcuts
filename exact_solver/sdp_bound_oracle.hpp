#pragma once

#include "graph.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace cutwidth::sdp {

enum class SdpSchedule { off, root, adaptive };

enum class SdpBoundStatus {
    certified,
    uncertified,
    cache_hit,
    disabled,
    ineligible,
    unavailable,
    busy,
    call_budget_exhausted,
    time_budget_exhausted,
    dimension_exceeded,
    invalid_request,
};

struct SdpBoundOracleOptions {
    SdpSchedule schedule = SdpSchedule::off;
    std::chrono::milliseconds total_time{0};
    std::size_t max_calls = 0;
    std::size_t max_state_dimension = 0;
    std::uint64_t trigger_nodes = 0;
    std::size_t max_iterations = 200;
    unsigned quantization_bits = 30;
};

struct SdpBoundRequest {
    std::span<const Graph::Mask> prefix;
    std::size_t cardinality = 0;
    std::uint64_t accumulated_subtree_nodes = 0;
    std::uint32_t existing_certified_bound = 0;
    bool root = false;
    std::chrono::steady_clock::time_point caller_deadline =
        std::chrono::steady_clock::time_point::max();
};

struct SdpBoundResult {
    SdpBoundStatus status = SdpBoundStatus::ineligible;
    std::optional<std::uint32_t> certified_lower_bound;
    double raw_objective = 0.0;
    double primal_residual = 0.0;
    double dual_residual = 0.0;
    std::size_t iterations = 0;
    double solve_seconds = 0.0;
    double certification_seconds = 0.0;
    std::string proof_kind;
    std::string graph_hash;
    std::string model_hash;
    std::string backend_hash;
    std::string diagnostic;
    bool cache_hit = false;
};

struct SdpBoundOracleStats {
    std::uint64_t requests = 0;
    std::uint64_t eligible = 0;
    std::uint64_t calls = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t certified = 0;
    std::uint64_t improvements = 0;
    std::uint64_t busy = 0;
    std::uint64_t budget_rejections = 0;
    std::uint64_t uncertified = 0;
    std::uint64_t dimension_rejections = 0;
    std::size_t preferred_max_dimension = 0;
    double backend_seconds = 0.0;
};

// Thread-safe, threshold-independent oracle. Expensive backend calls are
// serialized and never waited on: a concurrent DFS worker receives `busy`.
class SdpBoundOracle {
public:
    SdpBoundOracle(const Graph& graph, SdpBoundOracleOptions options);

    [[nodiscard]] SdpBoundResult bound(const SdpBoundRequest& request);
    [[nodiscard]] SdpBoundOracleStats stats() const;
    [[nodiscard]] const std::string& graph_hash() const noexcept { return graph_hash_; }
    [[nodiscard]] std::uint64_t trigger_nodes() const noexcept {
        return options_.trigger_nodes;
    }
    [[nodiscard]] bool should_attempt(std::size_t state_dimension,
                                      std::uint64_t accumulated_nodes) const noexcept;

private:
    struct CachedBound {
        std::uint32_t lower_bound = 0;
        std::string proof_kind;
        std::string model_hash;
    };

    const Graph& graph_;
    SdpBoundOracleOptions options_;
    std::string graph_hash_;
    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, CachedBound> cache_;
    std::mutex backend_mutex_;
    std::atomic<std::uint64_t> requests_{0}, eligible_{0}, calls_{0};
    std::atomic<std::uint64_t> cache_hits_{0}, certified_{0}, improvements_{0};
    std::atomic<std::uint64_t> busy_{0}, budget_rejections_{0};
    std::atomic<std::uint64_t> uncertified_{0}, dimension_rejections_{0};
    std::atomic<std::uint64_t> backend_nanoseconds_{0};
    std::atomic<std::size_t> preferred_max_dimension_{0};
};

} // namespace cutwidth::sdp
