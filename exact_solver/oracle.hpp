#pragma once

#include "graph.hpp"

#include <cstdint>
#include <vector>

namespace cutwidth::oracle {

struct OracleResult {
    std::uint32_t cutwidth = 0;
    std::vector<std::uint32_t> ordering;
};

// Independent factorial-time reference implementation. Intended for n <= 10.
OracleResult brute_force(const Graph& graph);

// Exact O(n 2^n)-time, O(2^n)-space subset dynamic program. Intended for
// moderate instances; the implementation rejects n > 25 to avoid accidental
// excessive allocation.
OracleResult subset_dp(const Graph& graph);

}  // namespace cutwidth::oracle
