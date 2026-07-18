#include "memory_governor.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <utility>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace cutwidth {

MemoryGovernor::Lease::~Lease() { reset(); }
MemoryGovernor::Lease::Lease(Lease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      consumer_(std::move(other.consumer_)),
      bytes_(std::exchange(other.bytes_, 0)) {}
MemoryGovernor::Lease& MemoryGovernor::Lease::operator=(Lease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    owner_ = std::exchange(other.owner_, nullptr);
    consumer_ = std::move(other.consumer_);
    bytes_ = std::exchange(other.bytes_, 0);
    return *this;
}
void MemoryGovernor::Lease::reset() noexcept {
    if (owner_) owner_->release(consumer_, bytes_);
    owner_ = nullptr;
    bytes_ = 0;
    consumer_.clear();
}

MemoryGovernor::MemoryGovernor(std::size_t budget_bytes,
                               std::optional<std::size_t> baseline_rss) {
    stats_.budget_bytes = budget_bytes;
    stats_.baseline_rss_bytes = baseline_rss.value_or(process_rss_bytes());
    stats_.sampled_rss_bytes = stats_.baseline_rss_bytes;
}

std::optional<MemoryGovernor::Lease> MemoryGovernor::try_acquire(
    std::string consumer, std::size_t bytes) {
    std::lock_guard lock(mutex_);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const bool overflow = bytes > maximum - stats_.sampled_rss_bytes ||
        bytes > maximum - stats_.baseline_rss_bytes ||
        stats_.committed_lease_bytes > maximum - bytes ||
        stats_.baseline_rss_bytes + bytes > maximum - stats_.committed_lease_bytes;
    if (overflow || stats_.memory_pressure) {
        ++stats_.leases_rejected;
        return std::nullopt;
    }
    const auto rss_projection = stats_.sampled_rss_bytes + bytes;
    const auto lease_projection = stats_.baseline_rss_bytes +
        stats_.committed_lease_bytes + bytes;
    const auto projected = std::max(rss_projection, lease_projection);
    if (stats_.budget_bytes != 0 && projected > stats_.budget_bytes) {
        ++stats_.leases_rejected;
        return std::nullopt;
    }
    stats_.committed_lease_bytes += bytes;
    committed_by_consumer_[consumer] += bytes;
    ++stats_.leases_granted;
    refresh_untracked_locked();
    return Lease(this, std::move(consumer), bytes);
}

std::size_t MemoryGovernor::sample_rss() {
    const auto rss = process_rss_bytes();
    std::lock_guard lock(mutex_);
    stats_.sampled_rss_bytes = rss;
    ++stats_.rss_samples;
    refresh_untracked_locked();
    if (stats_.budget_bytes != 0 && rss > stats_.budget_bytes)
        stats_.memory_pressure = true;
    return rss;
}

MemoryGovernorStats MemoryGovernor::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

std::size_t MemoryGovernor::committed_for(const std::string& consumer) const {
    std::lock_guard lock(mutex_);
    const auto found = committed_by_consumer_.find(consumer);
    return found == committed_by_consumer_.end() ? 0 : found->second;
}

void MemoryGovernor::report_memory_pressure() {
    std::lock_guard lock(mutex_);
    stats_.memory_pressure = true;
}

void MemoryGovernor::release(const std::string& consumer, std::size_t bytes) noexcept {
    std::lock_guard lock(mutex_);
    auto found = committed_by_consumer_.find(consumer);
    if (found == committed_by_consumer_.end() || found->second < bytes ||
        stats_.committed_lease_bytes < bytes) {
        stats_.memory_pressure = true;
        return;
    }
    found->second -= bytes;
    stats_.committed_lease_bytes -= bytes;
    if (found->second == 0) committed_by_consumer_.erase(found);
    refresh_untracked_locked();
}

void MemoryGovernor::refresh_untracked_locked() noexcept {
    const auto expected = stats_.committed_lease_bytes >
            std::numeric_limits<std::size_t>::max() - stats_.baseline_rss_bytes
        ? std::numeric_limits<std::size_t>::max()
        : stats_.baseline_rss_bytes + stats_.committed_lease_bytes;
    stats_.untracked_bytes = stats_.sampled_rss_bytes > expected
        ? stats_.sampled_rss_bytes - expected : 0;
}

std::size_t MemoryGovernor::process_rss_bytes() noexcept {
#ifdef __APPLE__
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<std::size_t>(info.resident_size);
#elif defined(__linux__)
    std::ifstream input("/proc/self/statm");
    std::size_t pages = 0, resident = 0;
    if (input >> pages >> resident) {
        const auto page_size = ::sysconf(_SC_PAGESIZE);
        if (page_size > 0 && resident <= std::numeric_limits<std::size_t>::max() /
                                      static_cast<std::size_t>(page_size))
            return resident * static_cast<std::size_t>(page_size);
    }
#endif
    return 0;
}

} // namespace cutwidth
