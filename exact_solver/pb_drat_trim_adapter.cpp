#include "pb_drat_trim_adapter.hpp"
#include <mutex>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
int run_drat_trim_in_memory(
    const unsigned char *cnf_data, size_t cnf_size,
    const unsigned char *proof_data, size_t proof_size,
    int timeout_seconds,
    int (*cancelled)(void *), void *cancel_state
);
}

namespace {
using Clock = std::chrono::steady_clock;

struct CancellationContext {
    const std::atomic<bool>* stop_requested = nullptr;
    Clock::time_point deadline = Clock::time_point::max();
};

int cancellation_requested(void* state) {
    const auto* context = static_cast<const CancellationContext*>(state);
    if (!context) return 0;
    if (context->stop_requested &&
        context->stop_requested->load(std::memory_order_relaxed)) return 1;
    return Clock::now() >= context->deadline ? 1 : 0;
}
}

namespace cutwidth::pb {

// The pinned checker has process-global scratch state.  Serialize calls, but
// make waiting deadline- and cancellation-aware so queued sessions fail
// closed instead of silently extending their budget.
static std::timed_mutex drat_checker_mutex;

DratCheckerResult verify_proof_in_memory(
    const CnfFormula& formula,
    const std::vector<std::int32_t>& added_unit_clauses,
    const std::vector<std::uint8_t>& proof_bytes,
    std::chrono::milliseconds timeout,
    const std::atomic<bool>* stop_requested
) {
    DratCheckerResult result;
    const auto started = Clock::now();
    CancellationContext cancellation{
        stop_requested,
        timeout.count() == 0 ? Clock::time_point::max() : started + timeout
    };
    if (cancellation_requested(&cancellation)) {
        result.status = DratCheckerStatus::timeout;
        return result;
    }
    if (proof_bytes.empty()) {
        result.status = DratCheckerStatus::not_verified;
        return result;
    }

    // 1. Generate DIMACS format string for the formula + added unit clauses
    std::uint32_t max_var = formula.variable_count;
    for (std::int32_t lit : added_unit_clauses) {
        if (lit == 0 || lit == std::numeric_limits<std::int32_t>::min()) {
            result.status = DratCheckerStatus::error;
            return result;
        }
        const auto var = static_cast<std::uint32_t>(
            lit < 0 ? -static_cast<std::int64_t>(lit) : lit);
        if (var > max_var) max_var = var;
    }

    std::ostringstream out;
    out << "p cnf " << max_var << ' ' << (formula.clauses.size() + added_unit_clauses.size()) << '\n';
    for (const auto& clause : formula.clauses) {
        if (cancellation_requested(&cancellation)) {
            result.status = DratCheckerStatus::timeout;
            return result;
        }
        for (const auto literal : clause) {
            out << literal << ' ';
        }
        out << "0\n";
    }
    for (std::int32_t lit : added_unit_clauses) {
        out << lit << " 0\n";
    }

    std::string dimacs_str = out.str();

    // Keep the upstream whole-second timeout as a backstop; the callback
    // above enforces the caller's millisecond deadline.
    const int timeout_seconds = timeout.count() == 0
        ? std::numeric_limits<int>::max()
        : static_cast<int>(std::min<std::int64_t>(
            std::numeric_limits<int>::max(), (timeout.count() + 999) / 1000));

    // 2. Serialize verification execution with mutex
    int status_code = -1;
    std::unique_lock<std::timed_mutex> lock(drat_checker_mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(10))) {
        if (cancellation_requested(&cancellation)) {
            result.status = DratCheckerStatus::timeout;
            result.runtime_seconds = std::chrono::duration<double>(
                Clock::now() - started).count();
            return result;
        }
    }
    status_code = run_drat_trim_in_memory(
        reinterpret_cast<const unsigned char*>(dimacs_str.data()), dimacs_str.size(),
        proof_bytes.data(), proof_bytes.size(),
        timeout_seconds, cancellation_requested, &cancellation
    );
    result.runtime_seconds = std::chrono::duration<double>(
        Clock::now() - started).count();

    // 3. Decode status_code
    if (status_code == 0) {
        result.status = DratCheckerStatus::verified;
    } else if (status_code == 100 || status_code == 102) {
        result.status = DratCheckerStatus::timeout;
    } else if (status_code == 1) {
        result.status = DratCheckerStatus::not_verified;
    } else {
        result.status = DratCheckerStatus::error;
    }

    return result;
}

} // namespace cutwidth::pb
