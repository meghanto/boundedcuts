#include "oracle.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace cutwidth::oracle {

OracleResult brute_force(const Graph& graph) {
    if (graph.size() > 10) {
        throw std::invalid_argument("brute-force oracle is limited to 10 vertices");
    }

    std::vector<std::uint32_t> permutation(graph.size());
    for (std::uint32_t v = 0; v < graph.size(); ++v) {
        permutation[v] = v;
    }

    OracleResult best;
    best.cutwidth = std::numeric_limits<std::uint32_t>::max();
    do {
        const auto value = graph.ordering_cutwidth(permutation);
        if (value < best.cutwidth) {
            best = {value, permutation};
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));

    // next_permutation executes once for the empty graph as well.
    if (best.cutwidth == std::numeric_limits<std::uint32_t>::max()) {
        best.cutwidth = 0;
    }
    return best;
}

OracleResult subset_dp(const Graph& graph) {
    const std::size_t n = graph.size();
    if (n > 25) {
        throw std::invalid_argument("subset-DP oracle is limited to 25 vertices");
    }
    const std::uint64_t states = std::uint64_t{1} << n;
    std::vector<std::uint32_t> dp(states, std::numeric_limits<std::uint32_t>::max());
    std::vector<std::uint8_t> parent(states, 0);
    dp[0] = 0;

    for (std::uint64_t set = 1; set < states; ++set) {
        const auto boundary = graph.cut(set);
        std::uint64_t remaining = set;
        while (remaining != 0) {
            const auto v = static_cast<std::uint32_t>(std::countr_zero(remaining));
            const auto previous = set & ~(std::uint64_t{1} << v);
            const auto candidate = std::max(dp[previous], boundary);
            if (candidate < dp[set]) {
                dp[set] = candidate;
                parent[set] = static_cast<std::uint8_t>(v);
            }
            remaining &= remaining - 1;
        }
    }

    OracleResult result;
    result.cutwidth = dp[states - 1];
    result.ordering.resize(n);
    auto set = states - 1;
    for (std::size_t position = n; position > 0; --position) {
        const auto v = static_cast<std::uint32_t>(parent[set]);
        result.ordering[position - 1] = v;
        set &= ~(std::uint64_t{1} << v);
    }
    return result;
}

}  // namespace cutwidth::oracle
