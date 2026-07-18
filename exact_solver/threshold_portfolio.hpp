#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cutwidth {

enum class ThresholdReason { ub_minus_one, geometric_ub_bias, midpoint };

enum class ValueAwareRebaseReason { none, initialized, upper_changed, depleted };

struct ValueAwareEpochUpdate {
    std::vector<std::uint32_t> active_candidates;
    ValueAwareRebaseReason reason = ValueAwareRebaseReason::none;

    [[nodiscard]] bool rebased() const noexcept {
        return reason != ValueAwareRebaseReason::none;
    }
};

struct ThresholdChoice {
    std::uint32_t threshold = 0;
    ThresholdReason reason = ThresholdReason::ub_minus_one;
};

class ThresholdPortfolio {
public:
    [[nodiscard]] std::optional<ThresholdChoice> next(
        std::uint32_t lower_bound, std::uint32_t upper_bound);
    // Called after the selected session received service without resolving.
    void note_censored_service() noexcept;
    // Any independently certified interval change restarts at UB-1.
    void reset() noexcept;

private:
    std::uint32_t remembered_lower_ = 0;
    std::uint32_t remembered_upper_ = 0;
    std::uint64_t offset_ = 1;
    bool initialized_ = false;
    bool midpoint_phase_ = false;
};

// Builds the stable recurring exact-session order for one live interval:
// primary UB-1, immediate lower frontier, UB-biased geometric probes,
// midpoint, then any still-relevant retained sessions.
[[nodiscard]] std::vector<std::uint32_t> persistent_threshold_candidates(
    std::uint32_t lower_bound, std::uint32_t upper_bound,
    std::span<const std::uint32_t> retained = {});

// Selects the first candidate at the minimum completed recurrence level.
// Advancing the returned candidate once yields level-synchronous universal
// service: no candidate can enter level L+1 before all candidates finish L.
[[nodiscard]] std::size_t select_recurring_threshold(
    std::span<const std::uint64_t> completed_recurrences);

// Candidates generator for the value-aware scheduler.
[[nodiscard]] std::vector<std::uint32_t> value_aware_threshold_candidates(
    std::uint32_t lower_bound, std::uint32_t upper_bound,
    std::span<const std::uint32_t> retained = {});

// Owns one stable value-aware candidate cohort. Lower-bound movement filters
// the cohort but does not synthesize a new lower-endpoint arm. A new cohort is
// created only after upper-bound progress or when fewer than two live cohort
// thresholds remain.
class ValueAwareThresholdEpoch {
public:
    [[nodiscard]] ValueAwareEpochUpdate update(
        std::uint32_t current_lower, std::uint32_t current_upper,
        std::span<const std::uint32_t> retained = {});
    void restore(std::uint32_t epoch_lower, std::uint32_t epoch_upper,
                 std::vector<std::uint32_t> candidates);

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] std::uint32_t lower() const noexcept { return lower_; }
    [[nodiscard]] std::uint32_t upper() const noexcept { return upper_; }
    [[nodiscard]] const std::vector<std::uint32_t>& candidates() const noexcept {
        return candidates_;
    }

private:
    void rebase(std::uint32_t lower, std::uint32_t upper,
                std::span<const std::uint32_t> retained);
    [[nodiscard]] std::vector<std::uint32_t> active(
        std::uint32_t lower, std::uint32_t upper) const;

    std::uint32_t lower_ = 0;
    std::uint32_t upper_ = 0;
    std::vector<std::uint32_t> candidates_;
    bool initialized_ = false;
};

[[nodiscard]] const char* value_aware_rebase_reason_name(
    ValueAwareRebaseReason reason) noexcept;

// Feasibility probability helper.
[[nodiscard]] double get_prior_feasible_probability(
    std::uint32_t lower_bound, std::uint32_t upper_bound, std::uint32_t K);

// Prior cost helper.
[[nodiscard]] double get_prior_cost(std::uint32_t lower_bound, std::uint32_t K);

} // namespace cutwidth
