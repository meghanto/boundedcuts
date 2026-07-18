#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace cutwidth {

struct MemoryGovernorStats {
    std::size_t budget_bytes = 0;
    std::size_t baseline_rss_bytes = 0;
    std::size_t sampled_rss_bytes = 0;
    std::size_t committed_lease_bytes = 0;
    std::size_t untracked_bytes = 0;
    std::uint64_t leases_granted = 0;
    std::uint64_t leases_rejected = 0;
    std::uint64_t rss_samples = 0;
    bool memory_pressure = false;
};

class MemoryGovernor {
public:
    class Lease {
    public:
        Lease() = default;
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
        [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
        [[nodiscard]] const std::string& consumer() const noexcept { return consumer_; }
        void reset() noexcept;

    private:
        friend class MemoryGovernor;
        Lease(MemoryGovernor* owner, std::string consumer, std::size_t bytes)
            : owner_(owner), consumer_(std::move(consumer)), bytes_(bytes) {}
        MemoryGovernor* owner_ = nullptr;
        std::string consumer_;
        std::size_t bytes_ = 0;
    };

    // Zero is an unlimited declared budget, useful for compatibility mode.
    explicit MemoryGovernor(std::size_t budget_bytes,
                            std::optional<std::size_t> baseline_rss = std::nullopt);
    [[nodiscard]] std::optional<Lease> try_acquire(std::string consumer,
                                                   std::size_t bytes);
    // Samples process RSS and freezes optional growth when it exceeds the
    // declared budget. Returns the sampled value.
    std::size_t sample_rss();
    [[nodiscard]] MemoryGovernorStats stats() const;
    [[nodiscard]] std::size_t committed_for(const std::string& consumer) const;
    void report_memory_pressure();

    [[nodiscard]] static std::size_t process_rss_bytes() noexcept;

private:
    void release(const std::string& consumer, std::size_t bytes) noexcept;
    void refresh_untracked_locked() noexcept;

    mutable std::mutex mutex_;
    MemoryGovernorStats stats_;
    std::unordered_map<std::string, std::size_t> committed_by_consumer_;
};

} // namespace cutwidth
