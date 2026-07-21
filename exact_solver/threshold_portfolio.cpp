#include "threshold_portfolio.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace cutwidth {

std::optional<ThresholdChoice> ThresholdPortfolio::next(
    std::uint32_t lower, std::uint32_t upper) {
    if (lower > upper) throw std::invalid_argument("lower bound exceeds upper bound");
    if (lower == upper) return std::nullopt;
    if (!initialized_ || lower != remembered_lower_ || upper != remembered_upper_) {
        remembered_lower_ = lower;
        remembered_upper_ = upper;
        offset_ = 1;
        midpoint_phase_ = false;
        initialized_ = true;
    }
    const auto midpoint = lower + (upper - lower) / 2U;
    if (midpoint_phase_) return ThresholdChoice{midpoint, ThresholdReason::midpoint};
    const std::uint64_t candidate64 = static_cast<std::uint64_t>(upper) - offset_;
    if (candidate64 <= midpoint || candidate64 <= lower) {
        midpoint_phase_ = true;
        return ThresholdChoice{midpoint, ThresholdReason::midpoint};
    }
    return ThresholdChoice{static_cast<std::uint32_t>(candidate64),
        offset_ == 1 ? ThresholdReason::ub_minus_one
                     : ThresholdReason::geometric_ub_bias};
}

void ThresholdPortfolio::note_censored_service() noexcept {
    if (midpoint_phase_) return;
    if (offset_ > std::numeric_limits<std::uint64_t>::max() / 2U)
        midpoint_phase_ = true;
    else
        offset_ *= 2U;
}

void ThresholdPortfolio::reset() noexcept {
    initialized_ = false;
    offset_ = 1;
    midpoint_phase_ = false;
}

std::vector<std::uint32_t> persistent_threshold_candidates(
    std::uint32_t lower, std::uint32_t upper,
    std::span<const std::uint32_t> retained) {
    if (lower >= upper)
        throw std::invalid_argument("persistent threshold interval must be open");
    const auto primary = upper - 1U;
    const auto midpoint = lower + (upper - lower) / 2U;
    std::vector<std::uint32_t> result{primary};
    auto append = [&](std::uint32_t candidate) {
        if (candidate >= lower && candidate < upper &&
            std::find(result.begin(), result.end(), candidate) == result.end())
            result.push_back(candidate);
    };
    append(lower);
    for (std::uint64_t offset = 2; offset < upper; offset *= 2U) {
        const auto candidate = static_cast<std::uint32_t>(upper - offset);
        if (candidate <= midpoint || candidate <= lower) break;
        append(candidate);
        if (offset > std::numeric_limits<std::uint64_t>::max() / 2U) break;
    }
    append(midpoint);
    for (const auto candidate : retained) append(candidate);
    return result;
}

std::size_t select_recurring_threshold(
    std::span<const std::uint64_t> completed_recurrences) {
    if (completed_recurrences.empty())
        throw std::invalid_argument("recurring threshold set must be nonempty");
    return static_cast<std::size_t>(std::distance(
        completed_recurrences.begin(),
        std::min_element(completed_recurrences.begin(), completed_recurrences.end())));
}

std::uint32_t select_primary_first_threshold(
    std::uint32_t upper, std::span<const std::uint32_t> lower_live,
    std::uint64_t service_tick) {
    if (upper == 0) throw std::invalid_argument("primary-first upper bound must be positive");
    const auto primary = upper - 1U;
    if (lower_live.empty() || service_tick % 4U != 3U) return primary;
    // Session maps are intentionally hash-indexed. Normalize only this tiny
    // admitted set so a checkpoint/resume or allocator layout cannot change
    // the lower-threshold service order.
    std::vector<std::uint32_t> ordered(lower_live.begin(), lower_live.end());
    std::sort(ordered.begin(), ordered.end());
    return ordered[(service_tick / 4U) % ordered.size()];
}

std::vector<std::uint32_t> value_aware_threshold_candidates(
    std::uint32_t lower, std::uint32_t upper,
    std::span<const std::uint32_t> retained) {
    if (lower >= upper)
        throw std::invalid_argument("value-aware threshold interval must be open");

    std::vector<std::uint32_t> result;
    auto append = [&](std::uint32_t candidate) {
        if (candidate >= lower && candidate < upper &&
            std::find(result.begin(), result.end(), candidate) == result.end()) {
            result.push_back(candidate);
        }
    };

    // 1. Primary Upper Bound: b - 1
    append(upper - 1U);

    // 2. Upper-Adjacent Probes: b - 2, b - 3
    if (upper >= 2U) append(upper - 2U);
    if (upper >= 3U) append(upper - 3U);

    const auto midpoint = lower + (upper - lower) / 2U;

    // 3. Upper dyadic probes: b-4, b-8, ... strictly above the
    // midpoint. b-2 is already present as an adjacent probe.
    for (std::uint64_t offset = 4; offset < upper; offset *= 2U) {
        const auto candidate = static_cast<std::uint32_t>(upper - offset);
        if (candidate <= midpoint || candidate <= lower) break;
        append(candidate);
        if (offset > std::numeric_limits<std::uint64_t>::max() / 2U) break;
    }

    // 4. Interval midpoint.
    append(midpoint);

    // 5. Retained live sessions remain eligible without changing identity.
    for (const auto candidate : retained) {
        append(candidate);
    }

    return result;
}

std::vector<std::uint32_t> ValueAwareThresholdEpoch::active(
    std::uint32_t lower, std::uint32_t upper) const {
    std::vector<std::uint32_t> result;
    result.reserve(candidates_.size());
    for (const auto candidate : candidates_)
        if (candidate >= lower && candidate < upper)
            result.push_back(candidate);
    return result;
}

void ValueAwareThresholdEpoch::rebase(
    std::uint32_t lower, std::uint32_t upper,
    std::span<const std::uint32_t> retained) {
    candidates_ = value_aware_threshold_candidates(lower, upper, retained);
    lower_ = lower;
    upper_ = upper;
    initialized_ = true;
}

ValueAwareEpochUpdate ValueAwareThresholdEpoch::update(
    std::uint32_t current_lower, std::uint32_t current_upper,
    std::span<const std::uint32_t> retained) {
    if (current_lower >= current_upper)
        throw std::invalid_argument("value-aware epoch interval must be open");
    if (!initialized_) {
        rebase(current_lower, current_upper, retained);
        return {active(current_lower, current_upper),
                ValueAwareRebaseReason::initialized};
    }

    auto live = active(current_lower, current_upper);
    if (current_upper != upper_) {
        rebase(current_lower, current_upper, retained);
        return {active(current_lower, current_upper),
                ValueAwareRebaseReason::upper_changed};
    }
    if (live.size() < 2U &&
        (current_lower != lower_ || current_upper != upper_)) {
        rebase(current_lower, current_upper, retained);
        return {active(current_lower, current_upper),
                ValueAwareRebaseReason::depleted};
    }
    return {std::move(live), ValueAwareRebaseReason::none};
}

void ValueAwareThresholdEpoch::restore(
    std::uint32_t epoch_lower, std::uint32_t epoch_upper,
    std::vector<std::uint32_t> candidates) {
    if (epoch_lower >= epoch_upper || candidates.empty())
        throw std::invalid_argument("invalid value-aware epoch snapshot");
    std::vector<std::uint32_t> seen;
    seen.reserve(candidates.size());
    for (const auto candidate : candidates) {
        if (candidate < epoch_lower || candidate >= epoch_upper ||
            std::find(seen.begin(), seen.end(), candidate) != seen.end())
            throw std::invalid_argument("invalid value-aware epoch candidate");
        seen.push_back(candidate);
    }
    lower_ = epoch_lower;
    upper_ = epoch_upper;
    candidates_ = std::move(candidates);
    initialized_ = true;
}

const char* value_aware_rebase_reason_name(ValueAwareRebaseReason reason) noexcept {
    switch (reason) {
    case ValueAwareRebaseReason::none: return "none";
    case ValueAwareRebaseReason::initialized: return "initialized";
    case ValueAwareRebaseReason::upper_changed: return "upper_changed";
    case ValueAwareRebaseReason::depleted: return "depleted";
    }
    return "unknown";
}

double get_prior_feasible_probability(std::uint32_t a, std::uint32_t b, std::uint32_t K) {
    if (K >= b) return 1.0;
    if (K < a) return 0.0;
    double gamma = 0.7;
    double num = std::pow(gamma, b - K) - std::pow(gamma, b - a + 1);
    double den = 1.0 - std::pow(gamma, b - a + 1);
    if (den <= 0.0) return 0.0;
    return std::max(0.0, std::min(1.0, num / den));
}

double get_prior_cost(std::uint32_t a, std::uint32_t K) {
    (void)a;
    (void)K;
    return 1.0;
}

} // namespace cutwidth
