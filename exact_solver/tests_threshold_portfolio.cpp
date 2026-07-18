#include "threshold_portfolio.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        cutwidth::ThresholdPortfolio policy;
        auto choice = policy.next(3, 13);
        require(choice && choice->threshold == 12 &&
                choice->reason == cutwidth::ThresholdReason::ub_minus_one,
                "portfolio did not start at UB-1");
        policy.note_censored_service();
        choice = policy.next(3, 13);
        require(choice && choice->threshold == 11 &&
                choice->reason == cutwidth::ThresholdReason::geometric_ub_bias,
                "portfolio did not descend to UB-2");
        policy.note_censored_service();
        choice = policy.next(3, 13);
        require(choice && choice->threshold == 9, "portfolio did not descend to UB-4");
        policy.note_censored_service();
        choice = policy.next(3, 13);
        require(choice && choice->threshold == 8 &&
                choice->reason == cutwidth::ThresholdReason::midpoint,
                "portfolio did not fall back at the midpoint");

        choice = policy.next(9, 13);
        require(choice && choice->threshold == 12,
                "certified interval change did not reset UB bias");
        choice = policy.next(12, 13);
        require(choice && choice->threshold == 12,
                "gap-one interval did not select its only decision");
        require(!policy.next(13, 13), "closed interval produced a threshold");

        bool rejected = false;
        try { (void)policy.next(2, 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "inverted interval accepted");

        const std::vector<std::uint32_t> retained{13, 2, 15};
        const auto candidates = cutwidth::persistent_threshold_candidates(3, 16, retained);
        require(candidates == std::vector<std::uint32_t>({15, 3, 14, 12, 9, 13}),
                "persistent threshold priority is not primary, lower frontier, "
                "UB ladder, midpoint, retained");
        std::vector<std::uint64_t> levels(candidates.size(), 0);
        for (std::size_t dispatch = 0; dispatch < 100; ++dispatch) {
            const auto selected = cutwidth::select_recurring_threshold(levels);
            if (dispatch == 0)
                require(selected == 0, "primary did not win an equal-level tie");
            ++levels[selected];
            const auto [minimum, maximum] = std::minmax_element(
                levels.begin(), levels.end());
            require(*maximum - *minimum <= 1,
                    "geometric recurrence advanced while an exact arm was a level behind");
        }
        rejected = false;
        try { (void)cutwidth::select_recurring_threshold({}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "empty recurring threshold set accepted");
        std::cout << "All threshold portfolio tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "THRESHOLD PORTFOLIO TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
