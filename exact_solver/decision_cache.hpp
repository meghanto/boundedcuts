#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <memory>
#include <mutex>
#include <vector>

namespace cutwidth {

enum class CacheReplacementPolicy : std::uint8_t {
    freeze,
    generational_clock,
};

struct DecisionCacheOptions {
    // Zero means unlimited. The memory limit includes slots and dynamic keys.
    std::size_t max_entries = 0;
    std::size_t max_memory_bytes = std::size_t{2} * 1024U * 1024U * 1024U;
    CacheReplacementPolicy replacement = CacheReplacementPolicy::freeze;
    // Power-of-two packed-page ceiling used only by generational replacement.
    std::size_t replacement_page_capacity = std::size_t{1} << 18U;
};

struct DecisionCacheStats {
    std::uint64_t queries = 0;
    std::uint64_t hits = 0;
    std::uint64_t inserts = 0;
    std::uint64_t strengthenings = 0;
    std::uint64_t rejected_capacity = 0;
    std::uint64_t collisions = 0;
    std::uint64_t rehashes = 0;
    std::uint64_t segment_growths = 0;
    std::uint64_t lookup_probes = 0;
    std::uint64_t insertion_probes = 0;
    std::uint64_t probes_avoided_after_saturation = 0;
    std::uint64_t page_promotions = 0;
    std::uint64_t page_second_chances = 0;
    std::uint64_t pages_recycled = 0;
    std::uint64_t replacement_admissions = 0;
    std::uint64_t entries_evicted = 0;
    std::uint64_t evicted_depth_sum = 0;
    std::uint32_t maximum_evicted_depth = 0;
    std::size_t entries = 0;
    std::size_t capacity = 0;
    std::size_t memory_bytes = 0;
    double bytes_per_state = 0.0;
    bool saturated = false;
};

// A physical image of the compact fixed-threshold table.  Capacities are
// recorded separately because they determine the cache's memory accounting
// and future saturation behaviour after a warm restart.
struct FixedThresholdDynamicCacheSegmentSnapshot {
    std::vector<std::uint8_t> control;
    std::vector<std::uint32_t> dense_index;
    std::vector<std::uint64_t> keys;
    std::vector<std::uint64_t> bloom;
    std::size_t size = 0;
    std::size_t control_capacity = 0;
    std::size_t dense_index_capacity = 0;
    std::size_t keys_capacity = 0;
    std::size_t bloom_capacity = 0;
    std::uint64_t depth_sum = 0;
    std::uint32_t maximum_depth = 0;
    bool referenced = true;
};

struct FixedThresholdDynamicCacheSnapshot {
    std::size_t word_count = 0;
    std::uint32_t threshold = 0;
    DecisionCacheOptions options;
    DecisionCacheStats stats;
    std::size_t size = 0;
    std::size_t clock_hand = 0;
    std::vector<FixedThresholdDynamicCacheSegmentSnapshot> segments;
};

struct ShardedFixedThresholdDynamicCacheSnapshot {
    std::size_t word_count = 0;
    std::uint32_t threshold = 0;
    std::size_t shard_count = 0;
    std::vector<FixedThresholdDynamicCacheSnapshot> shards;
};

// Compact multiword proof cache for one immutable threshold. The threshold is
// cache identity, not per-entry payload. Hashes and fingerprints only select
// candidates; every hit compares the complete key.
class FixedThresholdDynamicCache {
public:
    FixedThresholdDynamicCache(std::size_t word_count, std::uint32_t threshold,
                               DecisionCacheOptions options = {});
    [[nodiscard]] bool proves_failed(std::span<const std::uint64_t> key,
                                     std::uint32_t threshold);
    bool record_failed(std::span<const std::uint64_t> key,
                       std::uint32_t threshold);
    [[nodiscard]] const DecisionCacheStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t word_count() const noexcept { return word_count_; }
    [[nodiscard]] std::uint32_t threshold() const noexcept { return threshold_; }
    [[nodiscard]] FixedThresholdDynamicCacheSnapshot snapshot() const;
    [[nodiscard]] static FixedThresholdDynamicCache restore(
        const FixedThresholdDynamicCacheSnapshot& snapshot);
    void clear();

private:
    friend class ShardedFixedThresholdDynamicCache;
    struct Segment {
        std::vector<std::uint8_t> control;
        std::vector<std::uint32_t> dense_index;
        std::vector<std::uint64_t> keys;
        std::vector<std::uint64_t> bloom;
        std::size_t size = 0;
        std::uint64_t depth_sum = 0;
        std::uint32_t maximum_depth = 0;
        bool referenced = true;
    };
    void validate(std::span<const std::uint64_t> key, std::uint32_t threshold) const;
    [[nodiscard]] bool key_equal(const Segment& segment, std::uint32_t dense_index,
                                 std::span<const std::uint64_t> key) const noexcept;
    [[nodiscard]] bool proves_failed_hashed(
        std::span<const std::uint64_t> key, std::uint32_t threshold,
        std::uint64_t hash);
    bool record_failed_hashed(std::span<const std::uint64_t> key,
                              std::uint32_t threshold, std::uint64_t hash);
    [[nodiscard]] std::size_t find(Segment& segment, std::span<const std::uint64_t> key,
                                   std::uint64_t hash, std::uint8_t fingerprint,
                                   bool insertion);
    [[nodiscard]] static bool bloom_maybe_contains(
        const Segment& segment, std::uint64_t hash) noexcept;
    static void bloom_insert(Segment& segment, std::uint64_t hash) noexcept;
    [[nodiscard]] bool ensure_capacity();
    [[nodiscard]] bool recycle_page();
    void clear_segment(Segment& segment) noexcept;
    [[nodiscard]] bool add_segment(std::size_t capacity);
    [[nodiscard]] bool fits_additional(std::size_t control_slots,
                                       std::size_t key_words) const noexcept;
    void refresh_stats() noexcept;

    std::size_t word_count_;
    std::uint32_t threshold_;
    DecisionCacheOptions options_;
    DecisionCacheStats stats_;
    std::vector<Segment> segments_;
    std::size_t size_ = 0;
    std::size_t clock_hand_ = 0;
};

class ShardedFixedThresholdDynamicCache {
public:
    ShardedFixedThresholdDynamicCache(std::size_t word_count,
                                      std::size_t shard_count,
                                      std::uint32_t threshold,
                                      DecisionCacheOptions options = {});
    [[nodiscard]] bool proves_failed(std::span<const std::uint64_t> key,
                                     std::uint32_t threshold);
    bool record_failed(std::span<const std::uint64_t> key,
                       std::uint32_t threshold);
    [[nodiscard]] DecisionCacheStats stats() const;
    [[nodiscard]] std::size_t word_count() const noexcept {
        return shards_.empty() ? 0 : shards_.front()->cache.word_count();
    }
    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }
    [[nodiscard]] ShardedFixedThresholdDynamicCacheSnapshot snapshot() const;
    [[nodiscard]] static ShardedFixedThresholdDynamicCache restore(
        const ShardedFixedThresholdDynamicCacheSnapshot& snapshot);
private:
    struct Shard {
        Shard(std::size_t words, std::uint32_t threshold, DecisionCacheOptions options)
            : cache(words, threshold, options) {}
        explicit Shard(FixedThresholdDynamicCache&& restored)
            : cache(std::move(restored)) {}
        mutable std::mutex mutex;
        FixedThresholdDynamicCache cache;
    };
    [[nodiscard]] std::size_t shard_for(std::uint64_t hash) const noexcept;
    std::vector<std::unique_ptr<Shard>> shards_;
};

class FixedThresholdWord64Cache {
public:
    explicit FixedThresholdWord64Cache(std::uint32_t threshold,
                                       DecisionCacheOptions options = {});
    [[nodiscard]] bool proves_failed(std::uint64_t key, std::uint32_t threshold);
    bool record_failed(std::uint64_t key, std::uint32_t threshold);
    [[nodiscard]] const DecisionCacheStats& stats() const noexcept { return stats_; }
    void clear();
private:
    [[nodiscard]] std::size_t find(std::uint64_t key, bool count_collisions) const;
    [[nodiscard]] bool reserve_for_insert();
    [[nodiscard]] bool rehash(std::size_t capacity);
    void refresh_stats() noexcept;
    std::uint32_t threshold_;
    DecisionCacheOptions options_;
    mutable DecisionCacheStats stats_;
    std::vector<std::uint8_t> control_;
    std::vector<std::uint64_t> keys_;
    std::size_t size_ = 0;
};

// A cache entry records max_failed[S]. It proves failure for a query threshold
// k exactly when k <= max_failed[S]. Existing entries may always be
// strengthened, including after the cache reaches its configured capacity.
class Word64DecisionCache {
public:
    explicit Word64DecisionCache(DecisionCacheOptions options = {});

    [[nodiscard]] bool proves_failed(std::uint64_t key, std::uint32_t threshold);
    // Returns true if the proof was retained (new, strengthened, or redundant).
    bool record_failed(std::uint64_t key, std::uint32_t threshold);
    [[nodiscard]] const DecisionCacheStats& stats() const noexcept { return stats_; }
    void clear();

private:
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t max_failed = 0;
        bool occupied = false;
    };

    [[nodiscard]] std::size_t find(std::uint64_t key, bool count_collisions);
    [[nodiscard]] bool ensure_insert_capacity();
    [[nodiscard]] bool rehash(std::size_t capacity);
    void refresh_stats() noexcept;

    DecisionCacheOptions options_;
    DecisionCacheStats stats_;
    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};

class ShardedDynamicDecisionCache;

class DynamicDecisionCache {
public:
    explicit DynamicDecisionCache(std::size_t word_count,
                                  DecisionCacheOptions options = {});

    [[nodiscard]] bool proves_failed(std::span<const std::uint64_t> key,
                                     std::uint32_t threshold);
    bool record_failed(std::span<const std::uint64_t> key, std::uint32_t threshold);
    [[nodiscard]] const DecisionCacheStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t word_count() const noexcept { return word_count_; }
    [[nodiscard]] static std::size_t minimum_table_memory_bytes() noexcept;
    void clear();

private:
    friend class ShardedDynamicDecisionCache;
    struct Slot {
        std::uint64_t hash = 0;
        std::size_t key_offset = 0;
        std::uint32_t max_failed = 0;
        bool occupied = false;
    };

    void validate_key(std::span<const std::uint64_t> key) const;
    [[nodiscard]] bool proves_failed_hashed(
        std::span<const std::uint64_t> key, std::uint32_t threshold,
        std::uint64_t hash);
    bool record_failed_hashed(std::span<const std::uint64_t> key,
                              std::uint32_t threshold, std::uint64_t hash);
    [[nodiscard]] bool key_equal(const Slot& slot, std::span<const std::uint64_t> key) const noexcept;
    [[nodiscard]] std::size_t find(std::uint64_t hash,
                                   std::span<const std::uint64_t> key,
                                   bool count_collisions);
    [[nodiscard]] bool ensure_insert_capacity();
    [[nodiscard]] bool rehash(std::size_t capacity);
    [[nodiscard]] bool insertion_fits_memory(std::size_t capacity) const noexcept;
    void refresh_stats() noexcept;

    std::size_t word_count_;
    DecisionCacheOptions options_;
    DecisionCacheStats stats_;
    std::vector<Slot> slots_;
    std::vector<std::uint64_t> keys_;
    std::size_t size_ = 0;
};

// Concurrent word64 cache with stable hash ownership and no global hot lock.
// Each subset belongs to exactly one independently locked flat-cache shard.
class ShardedWord64DecisionCache {
public:
    ShardedWord64DecisionCache(std::size_t shard_count,
                               DecisionCacheOptions options = {});
    [[nodiscard]] bool proves_failed(std::uint64_t key, std::uint32_t threshold);
    bool record_failed(std::uint64_t key, std::uint32_t threshold);
    [[nodiscard]] DecisionCacheStats stats() const;
    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }

private:
    struct Shard {
        explicit Shard(DecisionCacheOptions options) : cache(options) {}
        mutable std::mutex mutex;
        Word64DecisionCache cache;
    };
    [[nodiscard]] std::size_t shard_for(std::uint64_t key) const noexcept;
    std::vector<std::unique_ptr<Shard>> shards_;
};

class ShardedFixedThresholdWord64Cache {
public:
    ShardedFixedThresholdWord64Cache(std::size_t shard_count,
                                     std::uint32_t threshold,
                                     DecisionCacheOptions options = {});
    [[nodiscard]] bool proves_failed(std::uint64_t key, std::uint32_t threshold);
    bool record_failed(std::uint64_t key, std::uint32_t threshold);
    [[nodiscard]] DecisionCacheStats stats() const;
private:
    struct Shard {
        Shard(std::uint32_t threshold, DecisionCacheOptions options)
            : cache(threshold, options) {}
        mutable std::mutex mutex;
        FixedThresholdWord64Cache cache;
    };
    [[nodiscard]] std::size_t shard_for(std::uint64_t key) const noexcept;
    std::vector<std::unique_ptr<Shard>> shards_;
};

// Concurrent multiword cache for the dynamic backend.  The complete key is
// still checked inside the owning shard; its hash is used only to choose which
// independently locked flat table owns the state.
class ShardedDynamicDecisionCache {
public:
    ShardedDynamicDecisionCache(std::size_t word_count,
                                std::size_t shard_count,
                                DecisionCacheOptions options = {});
    [[nodiscard]] bool proves_failed(std::span<const std::uint64_t> key,
                                     std::uint32_t threshold);
    bool record_failed(std::span<const std::uint64_t> key,
                       std::uint32_t threshold);
    [[nodiscard]] DecisionCacheStats stats() const;
    [[nodiscard]] std::size_t word_count() const noexcept { return word_count_; }
    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }

private:
    struct Shard {
        Shard(std::size_t word_count, DecisionCacheOptions options)
            : cache(word_count, options) {}
        mutable std::mutex mutex;
        DynamicDecisionCache cache;
    };
    [[nodiscard]] std::size_t shard_for(std::uint64_t hash) const noexcept;
    std::size_t word_count_;
    std::vector<std::unique_ptr<Shard>> shards_;
};

} // namespace cutwidth
