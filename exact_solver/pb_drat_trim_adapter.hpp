#pragma once

#include "pb_encoding.hpp"
#include <cstdint>
#include <vector>
#include <chrono>
#include <atomic>

namespace cutwidth::pb {

enum class DratCheckerStatus {
    verified,
    not_verified,
    timeout,
    error
};

struct DratCheckerResult {
    DratCheckerStatus status = DratCheckerStatus::error;
    double runtime_seconds = 0.0;
};

DratCheckerResult verify_proof_in_memory(
    const CnfFormula& formula,
    const std::vector<std::int32_t>& added_unit_clauses,
    const std::vector<std::uint8_t>& proof_bytes,
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* stop_requested = nullptr
);

} // namespace cutwidth::pb
