#pragma once

#include "graph.hpp"

#include <cstdint>
#include <span>
#include <optional>

namespace cutwidth {

// Bermond et al. (2025), Eq. (6), rounded up because cutwidth is integral:
// ceil(m(m+n-1) / (2(n-1)^2)).
[[nodiscard]] std::uint32_t average_degree_lower_bound(
    std::size_t vertex_count, std::size_t edge_count);

// Exact fallback from Bermond et al., Theorems 6 and 8: T(C,n) is the sum of
// the C greatest independent-request-set weights. This is not the unavailable
// closed-form Theorem 12 implementation; preprocessing costs O(n^2 log n).
[[nodiscard]] std::uint64_t grooming_request_capacity(
    std::size_t vertex_count, std::uint32_t cut_capacity);
[[nodiscard]] std::uint32_t grooming_density_lower_bound(
    std::size_t vertex_count, std::size_t edge_count);

struct PartialBoundOptions {
    bool residual_degree = true;
    bool edge_distance_area = false;
    bool degree_distance_area = false;
    bool degeneracy = false;
    // Expensive area/degeneracy bounds run only near a tight realized cut.
    // Cheap residual-degree propagation remains available at every slack.
    std::uint32_t expensive_max_slack = 1;

    bool lagrangian_prefix_bound = false;
    std::uint32_t lagrangian_max_slack = 1;
    std::uint32_t lagrangian_max_residual = 256;
    std::uint32_t lagrangian_denominator = 2;
};

struct PartialBoundValues {
    std::uint32_t current_cut = 0;
    std::uint32_t residual_degree = 0;
    std::uint32_t edge_distance_area = 0;
    std::uint32_t degree_distance_area = 0;
    std::uint32_t degeneracy = 0;
    std::uint32_t combined = 0;
};

struct PartialBoundStats {
    std::uint64_t evaluations = 0;
    std::uint64_t residual_degree_evaluations = 0;
    std::uint64_t edge_distance_area_evaluations = 0;
    std::uint64_t degree_distance_area_evaluations = 0;
    std::uint64_t degeneracy_evaluations = 0;
    std::uint64_t residual_degree_prunes = 0;
    std::uint64_t edge_distance_area_prunes = 0;
    std::uint64_t degree_distance_area_prunes = 0;
    std::uint64_t degeneracy_prunes = 0;
    std::uint64_t residual_degree_session_ceiling_skips = 0;
    std::uint64_t degeneracy_session_ceiling_skips = 0;
    std::uint64_t expensive_slack_gate_skips = 0;

    std::uint64_t lagrangian_evaluations = 0;
    std::uint64_t lagrangian_mincuts = 0;
    std::uint64_t lagrangian_certified_prunes = 0;
    std::uint64_t lagrangian_slack_gate_skips = 0;
    std::uint64_t lagrangian_residual_gate_skips = 0;
    std::uint64_t lagrangian_ineligible_gate_skips = 0;
    std::uint64_t lagrangian_overflow_gate_skips = 0;
};

// Certified lower bounds on the maximum cut encountered by every completion of
// `prefix`. The current API is the specialized <=63-vertex backend; the
// evaluator deliberately owns no search state so a dynamic-mask adapter can
// provide the same quantities later.
class PartialBoundEvaluator {
public:
    explicit PartialBoundEvaluator(const Graph& graph,
                                   PartialBoundOptions options = {},
                                   std::optional<std::uint32_t> threshold = std::nullopt);

    [[nodiscard]] static std::uint32_t residual_degree_session_ceiling(
        const Graph& graph);
    [[nodiscard]] static std::uint32_t degeneracy_session_ceiling(
        const Graph& graph);
    void note_session_ceiling_skips(PartialBoundStats& stats) const noexcept;
    [[nodiscard]] bool residual_degree_enabled() const noexcept {
        return options_.residual_degree;
    }

    [[nodiscard]] PartialBoundValues evaluate(Graph::Mask prefix,
                                              std::uint32_t depth) const;
    [[nodiscard]] PartialBoundValues evaluate(std::span<const Graph::Mask> prefix,
                                              std::uint32_t depth) const;

    // Evaluates cheap-to-expensive and attributes a threshold rejection to the
    // first bound that proves it. This is only an instrumentation convenience;
    // it has exactly the same semantics as evaluate().combined > threshold.
    [[nodiscard]] bool exceeds(Graph::Mask prefix, std::uint32_t depth,
                               std::uint32_t threshold,
                               PartialBoundStats& stats) const;
    [[nodiscard]] bool exceeds(std::span<const Graph::Mask> prefix, std::uint32_t depth,
                               std::uint32_t threshold,
                               PartialBoundStats& stats) const;
    // Search-oriented variant: the caller already established that the
    // realized prefix cut is within the threshold.
    [[nodiscard]] bool completion_exceeds(std::span<const Graph::Mask> prefix,
                                          std::uint32_t depth,
                                          std::uint32_t threshold,
                                          PartialBoundStats& stats,
                                          std::optional<std::uint32_t>
                                              known_maximum_residual_degree = std::nullopt,
                                          std::optional<std::uint32_t>
                                              known_current_cut = std::nullopt) const;

private:
    const Graph& graph_;
    PartialBoundOptions options_;
    bool residual_degree_skipped_ = false;
    bool degeneracy_skipped_ = false;
    bool triangle_free_ = false;
};

} // namespace cutwidth
