#pragma once

#include "graph.hpp"

#include <cstdint>
#include <vector>
#include <span>
#include <optional>

namespace cutwidth {

struct LagrangianTelemetry {
    std::uint64_t calls = 0;
    std::uint64_t mincuts = 0;
    std::uint32_t best_cardinality = 0;
    int64_t best_numerator = 0;
    int64_t best_denominator = 1;
    std::uint32_t certified_bound = 0;
    bool overflow = false;
    bool ineligible = false;
};

class LagrangianPrefixBoundEvaluator {
public:
    explicit LagrangianPrefixBoundEvaluator(const Graph& graph);

    // Dynamic mask prefix (>64 vertices) API
    [[nodiscard]] LagrangianTelemetry evaluate(
        std::span<const Graph::Mask> prefix,
        // Search threshold K.  Evaluation may return early only after a
        // certified lower bound strictly greater than K is found.
        std::optional<std::uint32_t> threshold = std::nullopt,
        const std::vector<std::uint32_t>& selected_cardinalities = {},
        std::optional<std::uint32_t> lambda_denominator = std::nullopt,
        std::optional<std::vector<int64_t>> lambda_numerators = std::nullopt
    ) const;

    // Single-word Mask prefix API (<= 63 vertices)
    [[nodiscard]] LagrangianTelemetry evaluate(
        Graph::Mask prefix,
        std::optional<std::uint32_t> threshold = std::nullopt,
        const std::vector<std::uint32_t>& selected_cardinalities = {},
        std::optional<std::uint32_t> lambda_denominator = std::nullopt,
        std::optional<std::vector<int64_t>> lambda_numerators = std::nullopt
    ) const;

private:
    const Graph& graph_;
    std::uint32_t max_degree_ = 0;
};

} // namespace cutwidth
