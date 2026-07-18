#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace cutwidth {

// Duplicate claims park only after the waiter abandons all of its older
// claims. That prevents ownership cycles while allowing one continuation to
// finish a canonical subset shared by many ordered prefixes.
enum class OwnershipAcquire { acquired, duplicate, saturated };

struct CanonicalOwnershipStats {
    std::uint64_t acquired = 0;
    std::uint64_t duplicate_waits = 0;
    std::uint64_t saturated = 0;
    std::uint64_t failure_publications = 0;
    std::uint64_t abandoned = 0;
    std::uint64_t waiters_woken = 0;
    std::size_t entries = 0;
};

// Bounded advisory ownership. Missing, saturated, or abandoned entries never
// imply failure. A duplicate waits for publication or abandonment and retries;
// only the verified proof cache may prune it.
class CanonicalOwnershipTable {
public:
    CanonicalOwnershipTable(std::size_t word_count, std::size_t shard_count,
                            std::size_t max_entries);
    [[nodiscard]] OwnershipAcquire acquire(std::span<const std::uint64_t> key,
                                           std::uint64_t owner);
    void publish_failure(std::span<const std::uint64_t> key, std::uint64_t owner);
    void abandon(std::span<const std::uint64_t> key, std::uint64_t owner) noexcept;
    [[nodiscard]] std::vector<std::uint64_t> take_ready_waiters();
    [[nodiscard]] CanonicalOwnershipStats stats() const;
private:
    struct Entry {
        std::uint64_t hash = 0;
        std::vector<std::uint64_t> key;
        std::uint64_t owner = 0;
        std::vector<std::uint64_t> waiters;
    };
    struct Shard { mutable std::mutex mutex; std::vector<Entry> entries; };
    [[nodiscard]] std::size_t shard_for(std::uint64_t hash) const noexcept;
    void release(std::span<const std::uint64_t> key, std::uint64_t owner,
                 bool failed) noexcept;
    std::size_t word_count_, max_entries_per_shard_;
    std::vector<std::unique_ptr<Shard>> shards_;
    mutable std::mutex ready_mutex_;
    std::vector<std::uint64_t> ready_waiters_;
    mutable std::mutex stats_mutex_;
    CanonicalOwnershipStats stats_;
};

} // namespace cutwidth
