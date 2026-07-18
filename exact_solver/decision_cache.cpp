#include "decision_cache.hpp"

#include "vertex_set.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <stdexcept>

namespace cutwidth {
namespace {

constexpr std::size_t initial_capacity = 16;
constexpr std::uint8_t occupied_control = 0x80;

std::uint32_t key_depth(std::span<const std::uint64_t> key) noexcept {
    std::uint64_t total = 0;
    for (const auto word : key) total += std::popcount(word);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        total, std::numeric_limits<std::uint32_t>::max()));
}

std::uint64_t mix(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

template <typename T>
std::vector<T> restore_vector(const std::vector<T>& values, std::size_t capacity,
                              const char* name) {
    if (capacity < values.size())
        throw std::invalid_argument(std::string("cache snapshot ") + name +
                                    " capacity is smaller than its contents");
    std::vector<T> restored;
    restored.reserve(capacity);
    // Cache accounting is defined in terms of vector capacity.  A warm image
    // must therefore fail closed on an allocator that cannot reproduce it.
    if (restored.capacity() != capacity)
        throw std::invalid_argument(std::string("cache snapshot ") + name +
                                    " capacity cannot be restored exactly");
    restored.insert(restored.end(), values.begin(), values.end());
    return restored;
}

void validate_fixed_snapshot(const FixedThresholdDynamicCacheSnapshot& snapshot) {
    if (snapshot.word_count == 0)
        throw std::invalid_argument("fixed cache snapshot has zero word count");
    std::size_t total_size = 0;
    std::size_t expected_capacity = 0;
    std::size_t expected_memory = 0;
    if (snapshot.options.replacement != CacheReplacementPolicy::freeze &&
        snapshot.options.replacement != CacheReplacementPolicy::generational_clock)
        throw std::invalid_argument("fixed cache snapshot has invalid replacement policy");
    if (snapshot.options.replacement == CacheReplacementPolicy::generational_clock &&
        (snapshot.options.replacement_page_capacity < initial_capacity ||
         !std::has_single_bit(snapshot.options.replacement_page_capacity)))
        throw std::invalid_argument("fixed cache snapshot has invalid replacement page cap");
    for (const auto& segment : snapshot.segments) {
        if (segment.control.empty() || !std::has_single_bit(segment.control.size()) ||
            segment.control.size() != segment.dense_index.size() ||
            segment.bloom.size() != (segment.control.size() * 8U + 63U) / 64U)
            throw std::invalid_argument("fixed cache snapshot has invalid segment geometry");
        if (segment.size > std::numeric_limits<std::uint32_t>::max() ||
            segment.size > std::numeric_limits<std::size_t>::max() / snapshot.word_count ||
            segment.keys.size() != segment.size * snapshot.word_count)
            throw std::invalid_argument("fixed cache snapshot has invalid dense keys");
        if (segment.control_capacity < segment.control.size() ||
            segment.dense_index_capacity < segment.dense_index.size() ||
            segment.keys_capacity < segment.keys.size() ||
            segment.bloom_capacity < segment.bloom.size())
            throw std::invalid_argument("fixed cache snapshot has invalid vector capacity");
        std::vector<bool> dense_seen(segment.size, false);
        std::size_t occupied = 0;
        std::uint64_t depth_sum = 0;
        std::uint32_t maximum_depth = 0;
        for (std::size_t slot = 0; slot < segment.control.size(); ++slot) {
            if (segment.control[slot] == 0) continue;
            ++occupied;
            const auto dense = segment.dense_index[slot];
            if (dense >= segment.size || dense_seen[dense])
                throw std::invalid_argument("fixed cache snapshot has invalid dense index");
            dense_seen[dense] = true;
            const auto offset = static_cast<std::size_t>(dense) * snapshot.word_count;
            const auto depth = key_depth(std::span<const std::uint64_t>(
                segment.keys.data() + offset, snapshot.word_count));
            depth_sum += depth;
            maximum_depth = std::max(maximum_depth, depth);
        }
        if (occupied != segment.size)
            throw std::invalid_argument("fixed cache snapshot occupancy disagrees with size");
        if (segment.depth_sum != depth_sum || segment.maximum_depth != maximum_depth)
            throw std::invalid_argument("fixed cache snapshot depth metadata disagrees with keys");
        if (total_size > std::numeric_limits<std::size_t>::max() - segment.size ||
            expected_capacity > std::numeric_limits<std::size_t>::max() - segment.control.size())
            throw std::invalid_argument("fixed cache snapshot size overflow");
        total_size += segment.size;
        expected_capacity += segment.control.size();
        const auto add_memory = segment.control_capacity * sizeof(std::uint8_t) +
            segment.dense_index_capacity * sizeof(std::uint32_t) +
            segment.keys_capacity * sizeof(std::uint64_t) +
            segment.bloom_capacity * sizeof(std::uint64_t);
        if (expected_memory > std::numeric_limits<std::size_t>::max() - add_memory)
            throw std::invalid_argument("fixed cache snapshot memory overflow");
        expected_memory += add_memory;
    }
    if (snapshot.options.max_entries != 0 && total_size > snapshot.options.max_entries)
        throw std::invalid_argument("fixed cache snapshot exceeds entry budget");
    if (snapshot.options.max_memory_bytes != 0 &&
        expected_memory > snapshot.options.max_memory_bytes)
        throw std::invalid_argument("fixed cache snapshot exceeds memory budget");
    if (snapshot.size != total_size || snapshot.stats.entries != total_size ||
        snapshot.stats.capacity != expected_capacity ||
        snapshot.stats.memory_bytes != expected_memory)
        throw std::invalid_argument("fixed cache snapshot statistics disagree with layout");
    if (!snapshot.segments.empty() && snapshot.clock_hand >= snapshot.segments.size())
        throw std::invalid_argument("fixed cache snapshot clock hand is invalid");
    const auto expected_bytes = total_size == 0 ? 0.0 :
        static_cast<double>(expected_memory) / static_cast<double>(total_size);
    if (snapshot.stats.bytes_per_state != expected_bytes)
        throw std::invalid_argument("fixed cache snapshot bytes-per-state disagrees with layout");
}

} // namespace

bool below_load_limit(std::size_t entries, std::size_t capacity) noexcept;

FixedThresholdDynamicCache::FixedThresholdDynamicCache(
    std::size_t word_count, std::uint32_t threshold, DecisionCacheOptions options)
    : word_count_(word_count), threshold_(threshold), options_(options) {
    if (word_count_ == 0)
        throw std::invalid_argument("fixed dynamic cache requires at least one key word");
    if (options_.replacement == CacheReplacementPolicy::generational_clock &&
        (options_.replacement_page_capacity < initial_capacity ||
         !std::has_single_bit(options_.replacement_page_capacity)))
        throw std::invalid_argument(
            "generational cache page capacity must be a power of two at least 16");
    (void)add_segment(initial_capacity);
}

void FixedThresholdDynamicCache::validate(
    std::span<const std::uint64_t> key, std::uint32_t threshold) const {
    if (threshold != threshold_)
        throw std::invalid_argument("fixed dynamic cache queried at a different threshold");
    if (key.size() != word_count_)
        throw std::invalid_argument("fixed dynamic cache key has wrong word count");
}

bool FixedThresholdDynamicCache::key_equal(
    const Segment& segment, std::uint32_t dense_index,
    std::span<const std::uint64_t> key) const noexcept {
    const auto offset = static_cast<std::size_t>(dense_index) * word_count_;
    return offset <= segment.keys.size() && segment.keys.size() - offset >= word_count_ &&
        std::equal(key.begin(), key.end(),
            segment.keys.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::size_t FixedThresholdDynamicCache::find(
    Segment& segment, std::span<const std::uint64_t> key, std::uint64_t hash,
    std::uint8_t fingerprint, bool insertion) {
    if (segment.control.empty()) return 0;
    auto index = static_cast<std::size_t>(hash) & (segment.control.size() - 1U);
    while (segment.control[index] != 0) {
        if (insertion) ++stats_.insertion_probes;
        else ++stats_.lookup_probes;
        if (segment.control[index] == fingerprint &&
            key_equal(segment, segment.dense_index[index], key))
            return index;
        ++stats_.collisions;
        index = (index + 1U) & (segment.control.size() - 1U);
    }
    if (insertion) ++stats_.insertion_probes;
    else ++stats_.lookup_probes;
    return index;
}

bool FixedThresholdDynamicCache::bloom_maybe_contains(
    const Segment& segment, std::uint64_t hash) noexcept {
    if (segment.bloom.empty()) return false;
    const auto bits = segment.bloom.size() * 64U;
    const auto first = static_cast<std::size_t>(hash % bits);
    const auto second_hash = mix(hash ^ 0x9e3779b97f4a7c15ULL);
    const auto second = static_cast<std::size_t>(second_hash % bits);
    return (segment.bloom[first / 64U] & (std::uint64_t{1} << (first % 64U))) != 0 &&
        (segment.bloom[second / 64U] & (std::uint64_t{1} << (second % 64U))) != 0;
}

void FixedThresholdDynamicCache::bloom_insert(
    Segment& segment, std::uint64_t hash) noexcept {
    const auto bits = segment.bloom.size() * 64U;
    const auto first = static_cast<std::size_t>(hash % bits);
    const auto second_hash = mix(hash ^ 0x9e3779b97f4a7c15ULL);
    const auto second = static_cast<std::size_t>(second_hash % bits);
    segment.bloom[first / 64U] |= std::uint64_t{1} << (first % 64U);
    segment.bloom[second / 64U] |= std::uint64_t{1} << (second % 64U);
}

bool FixedThresholdDynamicCache::fits_additional(
    std::size_t control_slots, std::size_t key_words) const noexcept {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (control_slots > maximum / sizeof(std::uint32_t) ||
        key_words > maximum / sizeof(std::uint64_t)) return false;
    const auto controls = control_slots;
    const auto indices = control_slots * sizeof(std::uint32_t);
    const auto keys = key_words * sizeof(std::uint64_t);
    if (controls > maximum - indices || controls + indices > maximum - keys) return false;
    std::size_t used = 0;
    for (const auto& segment : segments_) {
        const auto segment_bytes = segment.control.capacity() * sizeof(std::uint8_t) +
            segment.dense_index.capacity() * sizeof(std::uint32_t) +
            segment.keys.capacity() * sizeof(std::uint64_t) +
            segment.bloom.capacity() * sizeof(std::uint64_t);
        if (segment_bytes > maximum - used) return false;
        used += segment_bytes;
    }
    if (control_slots > maximum / 8U) return false;
    const auto bloom_words = (control_slots * 8U + 63U) / 64U;
    if (bloom_words > maximum / sizeof(std::uint64_t)) return false;
    const auto bloom = bloom_words * sizeof(std::uint64_t);
    if (controls + indices + keys > maximum - bloom) return false;
    const auto additional = controls + indices + keys + bloom;
    return additional <= maximum - used &&
        (options_.max_memory_bytes == 0 || used + additional <= options_.max_memory_bytes);
}

bool FixedThresholdDynamicCache::add_segment(std::size_t capacity) {
    if (capacity < initial_capacity) capacity = initial_capacity;
    const auto retained = (capacity / 10U) * 7U + ((capacity % 10U) * 7U) / 10U;
    if (retained > std::numeric_limits<std::size_t>::max() / word_count_) return false;
    const auto key_words = retained * word_count_;
    if (!std::has_single_bit(capacity) ||
        !fits_additional(capacity, key_words)) return false;
    Segment segment;
    try {
        segment.control.assign(capacity, 0);
        segment.dense_index.assign(capacity, 0);
        segment.keys.reserve(key_words);
        segment.bloom.assign((capacity * 8U + 63U) / 64U, 0);
    } catch (const std::bad_alloc&) {
        return false;
    }
    try { segments_.push_back(std::move(segment)); }
    catch (const std::bad_alloc&) { return false; }
    if (segments_.size() > 1) ++stats_.segment_growths;
    refresh_stats();
    return true;
}

bool FixedThresholdDynamicCache::ensure_capacity() {
    if (options_.max_entries != 0 && size_ >= options_.max_entries) return false;
    if (size_ >= std::numeric_limits<std::uint32_t>::max()) return false;
    if (segments_.empty() && !add_segment(initial_capacity)) return false;
    auto& active = segments_.back();
    if (!below_load_limit(active.size + 1U, active.control.size())) {
        if (active.control.size() > std::numeric_limits<std::size_t>::max() / 2U ||
            (options_.replacement == CacheReplacementPolicy::freeze &&
             !add_segment(active.control.size() * 2U))) return false;
        if (options_.replacement == CacheReplacementPolicy::generational_clock) {
            const auto next = std::min(
                active.control.size() * 2U, options_.replacement_page_capacity);
            if (!add_segment(next)) return false;
        }
    }
    return true;
}

void FixedThresholdDynamicCache::clear_segment(Segment& segment) noexcept {
    size_ -= segment.size;
    std::fill(segment.control.begin(), segment.control.end(), 0);
    std::fill(segment.dense_index.begin(), segment.dense_index.end(), 0);
    segment.keys.clear();
    std::fill(segment.bloom.begin(), segment.bloom.end(), 0);
    segment.size = 0;
    segment.depth_sum = 0;
    segment.maximum_depth = 0;
    segment.referenced = true;
}

bool FixedThresholdDynamicCache::recycle_page() {
    if (segments_.empty()) return false;
    std::size_t largest = 0;
    for (const auto& segment : segments_)
        if (segment.size != 0) largest = std::max(largest, segment.control.size());
    if (largest == 0) return false;

    constexpr std::size_t cold_sample = 4;
    std::size_t best = segments_.size();
    std::size_t cold_seen = 0;
    // One service call performs at most one revolution. If every eligible
    // page was hot, this call only demotes pages and rejects its insertion;
    // ordinary cache queries then get a real interval in which to promote
    // reused pages before a later insertion chooses a still-cold victim.
    const auto maximum_visits = segments_.size();
    for (std::size_t visit = 0;
         visit < maximum_visits && cold_seen < cold_sample; ++visit) {
        const auto index = clock_hand_++ % segments_.size();
        auto& segment = segments_[index];
        if (segment.size == 0 || segment.control.size() != largest) continue;
        if (segment.referenced) {
            segment.referenced = false;
            ++stats_.page_second_chances;
            continue;
        }
        ++cold_seen;
        if (best == segments_.size() ||
            static_cast<long double>(segment.depth_sum) / segment.size <
                static_cast<long double>(segments_[best].depth_sum) /
                    segments_[best].size)
            best = index;
    }
    if (best == segments_.size()) return false;

    auto& victim = segments_[best];
    ++stats_.pages_recycled;
    stats_.entries_evicted += victim.size;
    stats_.evicted_depth_sum += victim.depth_sum;
    stats_.maximum_evicted_depth = std::max(
        stats_.maximum_evicted_depth, victim.maximum_depth);
    clear_segment(victim);
    if (best + 1U != segments_.size())
        std::swap(segments_[best], segments_.back());
    clock_hand_ %= segments_.size();
    refresh_stats();
    return true;
}

bool FixedThresholdDynamicCache::proves_failed(
    std::span<const std::uint64_t> key, std::uint32_t threshold) {
    return proves_failed_hashed(key, threshold, hash_words(key));
}

bool FixedThresholdDynamicCache::proves_failed_hashed(
    std::span<const std::uint64_t> key, std::uint32_t threshold,
    std::uint64_t hash) {
    validate(key, threshold);
    ++stats_.queries;
    if (segments_.empty()) return false;
    const auto fingerprint = static_cast<std::uint8_t>((hash >> 56U) | 1U);
    for (auto segment = segments_.rbegin(); segment != segments_.rend(); ++segment) {
        if (!bloom_maybe_contains(*segment, hash)) continue;
        const auto slot = find(*segment, key, hash, fingerprint, false);
        if (segment->control[slot] != 0) {
            ++stats_.hits;
            if (options_.replacement == CacheReplacementPolicy::generational_clock &&
                !segment->referenced) {
                segment->referenced = true;
                ++stats_.page_promotions;
            }
            return true;
        }
    }
    return false;
}

bool FixedThresholdDynamicCache::record_failed(
    std::span<const std::uint64_t> key, std::uint32_t threshold) {
    return record_failed_hashed(key, threshold, hash_words(key));
}

bool FixedThresholdDynamicCache::record_failed_hashed(
    std::span<const std::uint64_t> key, std::uint32_t threshold,
    std::uint64_t hash) {
    validate(key, threshold);
    const auto fingerprint = static_cast<std::uint8_t>((hash >> 56U) | 1U);
    // Check for an existing proof before considering growth. This both avoids
    // allocating a new segment for a duplicate and gives the active segment a
    // single probe chain instead of searching it again below.
    for (auto& segment : segments_) {
        if (!bloom_maybe_contains(segment, hash)) continue;
        const auto existing = find(segment, key, hash, fingerprint, true);
        if (segment.control[existing] != 0) {
            if (options_.replacement == CacheReplacementPolicy::generational_clock &&
                !segment.referenced) {
                segment.referenced = true;
                ++stats_.page_promotions;
            }
            return true;
        }
    }
    // At a fixed threshold no existing entry needs strengthening. Once growth
    // is frozen, reject before touching the insertion probe chain; queries keep
    // all retained proofs readable.
    if (stats_.saturated && options_.replacement == CacheReplacementPolicy::freeze) {
        ++stats_.rejected_capacity;
        ++stats_.probes_avoided_after_saturation;
        return false;
    }
    bool recycled = false;
    if (!ensure_capacity()) {
        stats_.saturated = true;
        if (options_.replacement == CacheReplacementPolicy::generational_clock)
            recycled = recycle_page();
        if (!recycled) {
            ++stats_.rejected_capacity;
            ++stats_.probes_avoided_after_saturation;
            return false;
        }
    }
    auto& active = segments_.back();
    const auto slot = find(active, key, hash, fingerprint, true);
    const auto dense = static_cast<std::uint32_t>(active.size);
    try {
        active.keys.insert(active.keys.end(), key.begin(), key.end());
    } catch (const std::bad_alloc&) {
        stats_.saturated = true;
        ++stats_.rejected_capacity;
        return false;
    }
    active.control[slot] = fingerprint;
    active.dense_index[slot] = dense;
    bloom_insert(active, hash);
    ++active.size;
    ++size_;
    const auto depth = key_depth(key);
    active.depth_sum += depth;
    active.maximum_depth = std::max(active.maximum_depth, depth);
    active.referenced = true;
    ++stats_.inserts;
    if (stats_.saturated &&
        options_.replacement == CacheReplacementPolicy::generational_clock)
        ++stats_.replacement_admissions;
    stats_.entries = size_;
    stats_.bytes_per_state = size_ == 0 ? 0.0 :
        static_cast<double>(stats_.memory_bytes) / static_cast<double>(size_);
    return true;
}

void FixedThresholdDynamicCache::refresh_stats() noexcept {
    stats_.entries = size_;
    stats_.capacity = 0;
    stats_.memory_bytes = 0;
    for (const auto& segment : segments_) {
        stats_.capacity += segment.control.size();
        stats_.memory_bytes += segment.control.capacity() * sizeof(std::uint8_t) +
            segment.dense_index.capacity() * sizeof(std::uint32_t) +
            segment.keys.capacity() * sizeof(std::uint64_t) +
            segment.bloom.capacity() * sizeof(std::uint64_t);
    }
    stats_.bytes_per_state = size_ == 0 ? 0.0 :
        static_cast<double>(stats_.memory_bytes) / static_cast<double>(size_);
}

FixedThresholdDynamicCacheSnapshot FixedThresholdDynamicCache::snapshot() const {
    FixedThresholdDynamicCacheSnapshot result;
    result.word_count = word_count_;
    result.threshold = threshold_;
    result.options = options_;
    result.stats = stats_;
    result.size = size_;
    result.clock_hand = clock_hand_;
    result.segments.reserve(segments_.size());
    for (const auto& segment : segments_) {
        FixedThresholdDynamicCacheSegmentSnapshot saved;
        saved.control = segment.control;
        saved.dense_index = segment.dense_index;
        saved.keys = segment.keys;
        saved.bloom = segment.bloom;
        saved.size = segment.size;
        saved.control_capacity = segment.control.capacity();
        saved.dense_index_capacity = segment.dense_index.capacity();
        saved.keys_capacity = segment.keys.capacity();
        saved.bloom_capacity = segment.bloom.capacity();
        saved.depth_sum = segment.depth_sum;
        saved.maximum_depth = segment.maximum_depth;
        saved.referenced = segment.referenced;
        result.segments.push_back(std::move(saved));
    }
    return result;
}

FixedThresholdDynamicCache FixedThresholdDynamicCache::restore(
    const FixedThresholdDynamicCacheSnapshot& snapshot) {
    validate_fixed_snapshot(snapshot);
    FixedThresholdDynamicCache result(snapshot.word_count, snapshot.threshold, snapshot.options);
    result.segments_.clear();
    result.segments_.reserve(snapshot.segments.size());
    for (const auto& saved : snapshot.segments) {
        Segment segment;
        segment.control = restore_vector(saved.control, saved.control_capacity, "control");
        segment.dense_index = restore_vector(saved.dense_index, saved.dense_index_capacity,
                                             "dense-index");
        segment.keys = restore_vector(saved.keys, saved.keys_capacity, "keys");
        segment.bloom = restore_vector(saved.bloom, saved.bloom_capacity, "bloom");
        segment.size = saved.size;
        segment.depth_sum = saved.depth_sum;
        segment.maximum_depth = saved.maximum_depth;
        segment.referenced = saved.referenced;
        result.segments_.push_back(std::move(segment));
    }
    result.size_ = snapshot.size;
    result.clock_hand_ = snapshot.clock_hand;
    result.stats_ = snapshot.stats;
    return result;
}

void FixedThresholdDynamicCache::clear() {
    stats_ = {};
    size_ = 0;
    segments_.clear();
    clock_hand_ = 0;
    (void)add_segment(initial_capacity);
}

ShardedFixedThresholdDynamicCache::ShardedFixedThresholdDynamicCache(
    std::size_t words, std::size_t shard_count, std::uint32_t threshold,
    DecisionCacheOptions options) {
    if (words == 0 || shard_count == 0)
        throw std::invalid_argument("sharded fixed dynamic cache dimensions must be nonzero");
    if (options.max_entries != 0) shard_count = std::min(shard_count, options.max_entries);
    // Each shard requires its initial packed table. Do not create empty shards
    // that already exceed the declared cache budget.
    constexpr std::size_t minimum_shard_bytes = initial_capacity *
        (sizeof(std::uint8_t) + sizeof(std::uint32_t));
    if (options.max_memory_bytes != 0)
        shard_count = std::min(shard_count,
            std::max<std::size_t>(1, options.max_memory_bytes / minimum_shard_bytes));
    shard_count = std::max<std::size_t>(1, shard_count);
    shards_.reserve(shard_count);
    for (std::size_t index = 0; index < shard_count; ++index) {
        auto local = options;
        if (local.max_entries != 0) {
            const auto base = local.max_entries / shard_count;
            local.max_entries = base + (index < local.max_entries % shard_count ? 1U : 0U);
        }
        if (local.max_memory_bytes != 0) {
            const auto base = local.max_memory_bytes / shard_count;
            local.max_memory_bytes = base +
                (index < local.max_memory_bytes % shard_count ? 1U : 0U);
        }
        shards_.push_back(std::make_unique<Shard>(words, threshold, local));
    }
}

std::size_t ShardedFixedThresholdDynamicCache::shard_for(
    std::uint64_t hash) const noexcept {
    return static_cast<std::size_t>(hash >> 32U) % shards_.size();
}

bool ShardedFixedThresholdDynamicCache::proves_failed(
    std::span<const std::uint64_t> key, std::uint32_t threshold) {
    const auto hash = hash_words(key);
    auto& shard = *shards_[shard_for(hash)];
    std::lock_guard lock(shard.mutex);
    return shard.cache.proves_failed_hashed(key, threshold, hash);
}

bool ShardedFixedThresholdDynamicCache::record_failed(
    std::span<const std::uint64_t> key, std::uint32_t threshold) {
    const auto hash = hash_words(key);
    auto& shard = *shards_[shard_for(hash)];
    std::lock_guard lock(shard.mutex);
    return shard.cache.record_failed_hashed(key, threshold, hash);
}

DecisionCacheStats ShardedFixedThresholdDynamicCache::stats() const {
    DecisionCacheStats total;
    for (const auto& owned : shards_) {
        std::lock_guard lock(owned->mutex);
        const auto& part = owned->cache.stats();
        total.queries += part.queries; total.hits += part.hits;
        total.inserts += part.inserts; total.rejected_capacity += part.rejected_capacity;
        total.collisions += part.collisions; total.rehashes += part.rehashes;
        total.segment_growths += part.segment_growths;
        total.lookup_probes += part.lookup_probes;
        total.insertion_probes += part.insertion_probes;
        total.probes_avoided_after_saturation += part.probes_avoided_after_saturation;
        total.page_promotions += part.page_promotions;
        total.page_second_chances += part.page_second_chances;
        total.pages_recycled += part.pages_recycled;
        total.replacement_admissions += part.replacement_admissions;
        total.entries_evicted += part.entries_evicted;
        total.evicted_depth_sum += part.evicted_depth_sum;
        total.maximum_evicted_depth = std::max(
            total.maximum_evicted_depth, part.maximum_evicted_depth);
        total.entries += part.entries; total.capacity += part.capacity;
        total.memory_bytes += part.memory_bytes;
        total.saturated = total.saturated || part.saturated;
    }
    total.bytes_per_state = total.entries == 0 ? 0.0 :
        static_cast<double>(total.memory_bytes) / static_cast<double>(total.entries);
    return total;
}

ShardedFixedThresholdDynamicCacheSnapshot ShardedFixedThresholdDynamicCache::snapshot() const {
    ShardedFixedThresholdDynamicCacheSnapshot result;
    result.shard_count = shards_.size();
    if (shards_.empty()) return result;
    result.word_count = shards_.front()->cache.word_count();
    result.threshold = shards_.front()->cache.threshold();
    result.shards.reserve(shards_.size());
    for (const auto& owned : shards_) {
        std::lock_guard lock(owned->mutex);
        result.shards.push_back(owned->cache.snapshot());
    }
    return result;
}

ShardedFixedThresholdDynamicCache ShardedFixedThresholdDynamicCache::restore(
    const ShardedFixedThresholdDynamicCacheSnapshot& snapshot) {
    if (snapshot.word_count == 0 || snapshot.shard_count == 0 ||
        snapshot.shards.size() != snapshot.shard_count)
        throw std::invalid_argument("sharded fixed cache snapshot has invalid dimensions");
    for (const auto& shard : snapshot.shards) {
        if (shard.word_count != snapshot.word_count || shard.threshold != snapshot.threshold)
            throw std::invalid_argument("sharded fixed cache snapshot has inconsistent identity");
        validate_fixed_snapshot(shard);
    }
    ShardedFixedThresholdDynamicCache result(snapshot.word_count, 1, snapshot.threshold);
    result.shards_.clear();
    result.shards_.reserve(snapshot.shard_count);
    for (const auto& shard : snapshot.shards)
        result.shards_.push_back(std::make_unique<Shard>(FixedThresholdDynamicCache::restore(shard)));
    return result;
}

bool below_load_limit(std::size_t entries, std::size_t capacity) noexcept {
    // entries/capacity <= 0.70 without floating point.
    return entries <= (capacity * 7U) / 10U;
}

FixedThresholdWord64Cache::FixedThresholdWord64Cache(
    std::uint32_t threshold, DecisionCacheOptions options)
    : threshold_(threshold), options_(options) { (void)rehash(initial_capacity); }

std::size_t FixedThresholdWord64Cache::find(std::uint64_t key,
                                             bool count_collisions) const {
    if (control_.empty()) return 0;
    std::size_t index = static_cast<std::size_t>(mix(key)) & (control_.size() - 1U);
    while (control_[index] == occupied_control && keys_[index] != key) {
        if (count_collisions) ++stats_.collisions;
        index = (index + 1U) & (control_.size() - 1U);
    }
    return index;
}

bool FixedThresholdWord64Cache::rehash(std::size_t capacity) {
    capacity = std::max(capacity, initial_capacity);
    constexpr std::size_t slot_bytes = sizeof(std::uint8_t) + sizeof(std::uint64_t);
    if (options_.max_memory_bytes != 0 && capacity > options_.max_memory_bytes / slot_bytes)
        return false;
    auto old_control = std::move(control_);
    auto old_keys = std::move(keys_);
    try {
        control_.assign(capacity, 0);
        keys_.assign(capacity, 0);
    } catch (const std::bad_alloc&) {
        control_ = std::move(old_control);
        keys_ = std::move(old_keys);
        return false;
    }
    for (std::size_t i = 0; i < old_control.size(); ++i) {
        if (old_control[i] != occupied_control) continue;
        const auto target = find(old_keys[i], false);
        control_[target] = occupied_control;
        keys_[target] = old_keys[i];
    }
    if (!old_control.empty()) ++stats_.rehashes;
    refresh_stats();
    return true;
}

bool FixedThresholdWord64Cache::reserve_for_insert() {
    if (options_.max_entries != 0 && size_ >= options_.max_entries) return false;
    if (control_.empty()) return rehash(initial_capacity);
    if (below_load_limit(size_ + 1U, control_.size())) return true;
    if (control_.size() > std::numeric_limits<std::size_t>::max() / 2U) return false;
    return rehash(control_.size() * 2U);
}

bool FixedThresholdWord64Cache::proves_failed(std::uint64_t key,
                                               std::uint32_t threshold) {
    ++stats_.queries;
    if (threshold != threshold_ || control_.empty()) return false;
    const bool hit = control_[find(key, true)] == occupied_control;
    if (hit) ++stats_.hits;
    return hit;
}

bool FixedThresholdWord64Cache::record_failed(std::uint64_t key,
                                               std::uint32_t threshold) {
    if (threshold != threshold_) return false;
    std::size_t index = control_.empty() ? 0 : find(key, true);
    if (!control_.empty() && control_[index] == occupied_control) return true;
    const auto old_capacity = control_.size();
    if (!reserve_for_insert()) {
        ++stats_.rejected_capacity;
        stats_.saturated = true;
        return false;
    }
    if (old_capacity != control_.size()) index = find(key, true);
    control_[index] = occupied_control;
    keys_[index] = key;
    ++size_;
    ++stats_.inserts;
    refresh_stats();
    return true;
}

void FixedThresholdWord64Cache::refresh_stats() noexcept {
    stats_.entries = size_;
    stats_.capacity = control_.size();
    stats_.memory_bytes = control_.capacity() + keys_.capacity() * sizeof(std::uint64_t);
    stats_.bytes_per_state = size_ == 0 ? 0.0 :
        static_cast<double>(stats_.memory_bytes) / static_cast<double>(size_);
}

void FixedThresholdWord64Cache::clear() {
    size_ = 0;
    stats_ = {};
    control_.clear();
    keys_.clear();
    (void)rehash(initial_capacity);
}

Word64DecisionCache::Word64DecisionCache(DecisionCacheOptions options) : options_(options) {
    (void)rehash(initial_capacity);
}

std::size_t Word64DecisionCache::find(std::uint64_t key, bool count_collisions) {
    if (slots_.empty()) return 0;
    std::size_t index = static_cast<std::size_t>(mix(key)) & (slots_.size() - 1U);
    while (slots_[index].occupied && slots_[index].key != key) {
        if (count_collisions) ++stats_.collisions;
        index = (index + 1U) & (slots_.size() - 1U);
    }
    return index;
}

bool Word64DecisionCache::proves_failed(std::uint64_t key, std::uint32_t threshold) {
    ++stats_.queries;
    if (slots_.empty()) return false;
    const Slot& slot = slots_[find(key, true)];
    const bool hit = slot.occupied && threshold <= slot.max_failed;
    if (hit) ++stats_.hits;
    return hit;
}

bool Word64DecisionCache::rehash(std::size_t capacity) {
    if (capacity < initial_capacity) capacity = initial_capacity;
    if (options_.max_memory_bytes != 0 && capacity > options_.max_memory_bytes / sizeof(Slot))
        return false;
    std::vector<Slot> old = std::move(slots_);
    try {
        slots_.assign(capacity, {});
    } catch (const std::bad_alloc&) {
        slots_ = std::move(old);
        return false;
    }
    for (const Slot& slot : old) {
        if (!slot.occupied) continue;
        slots_[find(slot.key, false)] = slot;
    }
    if (!old.empty()) ++stats_.rehashes;
    refresh_stats();
    return true;
}

bool Word64DecisionCache::ensure_insert_capacity() {
    if (options_.max_entries != 0 && size_ >= options_.max_entries) return false;
    if (slots_.empty()) return rehash(initial_capacity);
    if (below_load_limit(size_ + 1U, slots_.size())) return true;
    if (slots_.size() > std::numeric_limits<std::size_t>::max() / 2U) return false;
    return rehash(slots_.size() * 2U);
}

bool Word64DecisionCache::record_failed(std::uint64_t key, std::uint32_t threshold) {
    std::size_t insertion_index = 0;
    std::size_t old_capacity = slots_.size();
    if (!slots_.empty()) {
        insertion_index = find(key, true);
        Slot& existing = slots_[insertion_index];
        if (existing.occupied) {
            if (threshold > existing.max_failed) {
                existing.max_failed = threshold;
                ++stats_.strengthenings;
            }
            return true;
        }
    }
    if (!ensure_insert_capacity()) {
        ++stats_.rejected_capacity;
        stats_.saturated = true;
        return false;
    }
    // The first lookup already found the insertion slot. Reprobe only when
    // ensure_insert_capacity() rehashed the table and invalidated its index.
    if (old_capacity != slots_.size()) insertion_index = find(key, true);
    Slot& slot = slots_[insertion_index];
    slot = Slot{key, threshold, true};
    ++size_;
    ++stats_.inserts;
    refresh_stats();
    return true;
}

void Word64DecisionCache::refresh_stats() noexcept {
    stats_.entries = size_;
    stats_.capacity = slots_.size();
    stats_.memory_bytes = slots_.capacity() * sizeof(Slot);
}

void Word64DecisionCache::clear() {
    size_ = 0;
    stats_ = {};
    slots_.clear();
    (void)rehash(initial_capacity);
}

DynamicDecisionCache::DynamicDecisionCache(std::size_t word_count, DecisionCacheOptions options)
    : word_count_(word_count), options_(options) {
    if (word_count_ == 0) throw std::invalid_argument("dynamic cache requires at least one key word");
    (void)rehash(initial_capacity);
}

std::size_t DynamicDecisionCache::minimum_table_memory_bytes() noexcept {
    return initial_capacity * sizeof(Slot);
}

void DynamicDecisionCache::validate_key(std::span<const std::uint64_t> key) const {
    if (key.size() != word_count_) throw std::invalid_argument("dynamic cache key has wrong word count");
}

bool DynamicDecisionCache::key_equal(const Slot& slot,
                                     std::span<const std::uint64_t> key) const noexcept {
    return std::equal(key.begin(), key.end(), keys_.begin() + static_cast<std::ptrdiff_t>(slot.key_offset));
}

std::size_t DynamicDecisionCache::find(std::uint64_t hash,
                                       std::span<const std::uint64_t> key,
                                       bool count_collisions) {
    if (slots_.empty()) return 0;
    std::size_t index = static_cast<std::size_t>(hash) & (slots_.size() - 1U);
    while (slots_[index].occupied &&
           (slots_[index].hash != hash || !key_equal(slots_[index], key))) {
        if (count_collisions) ++stats_.collisions;
        index = (index + 1U) & (slots_.size() - 1U);
    }
    return index;
}

bool DynamicDecisionCache::proves_failed(std::span<const std::uint64_t> key,
                                         std::uint32_t threshold) {
    return proves_failed_hashed(key, threshold, hash_words(key));
}

bool DynamicDecisionCache::proves_failed_hashed(
    std::span<const std::uint64_t> key, std::uint32_t threshold,
    std::uint64_t hash) {
    validate_key(key);
    ++stats_.queries;
    if (slots_.empty()) return false;
    const Slot& slot = slots_[find(hash, key, true)];
    const bool hit = slot.occupied && threshold <= slot.max_failed;
    if (hit) ++stats_.hits;
    return hit;
}

bool DynamicDecisionCache::insertion_fits_memory(std::size_t capacity) const noexcept {
    if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(Slot) ||
        word_count_ > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) ||
        keys_.size() > std::numeric_limits<std::size_t>::max() - word_count_) return false;
    if (options_.max_memory_bytes == 0) return true;
    if (capacity > options_.max_memory_bytes / sizeof(Slot)) return false;
    const std::size_t slots_bytes = capacity * sizeof(Slot);
    if (word_count_ > (options_.max_memory_bytes - slots_bytes) / sizeof(std::uint64_t)) return false;
    const std::size_t key_bytes = word_count_ * sizeof(std::uint64_t);
    return keys_.size() <= (options_.max_memory_bytes - slots_bytes - key_bytes) /
                               sizeof(std::uint64_t);
}

bool DynamicDecisionCache::rehash(std::size_t capacity) {
    if (capacity < initial_capacity) capacity = initial_capacity;
    if (!insertion_fits_memory(capacity)) return false;
    std::vector<Slot> old = std::move(slots_);
    try {
        slots_.assign(capacity, {});
    } catch (const std::bad_alloc&) {
        slots_ = std::move(old);
        return false;
    }
    for (const Slot& slot : old) {
        if (!slot.occupied) continue;
        const std::span<const std::uint64_t> key(keys_.data() + slot.key_offset, word_count_);
        slots_[find(slot.hash, key, false)] = slot;
    }
    if (!old.empty()) ++stats_.rehashes;
    refresh_stats();
    return true;
}

bool DynamicDecisionCache::ensure_insert_capacity() {
    if (options_.max_entries != 0 && size_ >= options_.max_entries) return false;
    if (slots_.empty() && !rehash(initial_capacity)) return false;
    if (!below_load_limit(size_ + 1U, slots_.size())) {
        if (slots_.size() > std::numeric_limits<std::size_t>::max() / 2U ||
            !rehash(slots_.size() * 2U)) return false;
    }
    return insertion_fits_memory(slots_.size());
}

bool DynamicDecisionCache::record_failed(std::span<const std::uint64_t> key,
                                         std::uint32_t threshold) {
    return record_failed_hashed(key, threshold, hash_words(key));
}

bool DynamicDecisionCache::record_failed_hashed(
    std::span<const std::uint64_t> key, std::uint32_t threshold,
    std::uint64_t hash) {
    validate_key(key);
    std::size_t slot_index = 0;
    const std::size_t old_capacity = slots_.size();
    if (!slots_.empty()) {
        slot_index = find(hash, key, true);
        Slot& existing = slots_[slot_index];
        if (existing.occupied) {
            if (threshold > existing.max_failed) {
                existing.max_failed = threshold;
                ++stats_.strengthenings;
            }
            return true;
        }
    }
    if (!ensure_insert_capacity()) {
        ++stats_.rejected_capacity;
        stats_.saturated = true;
        return false;
    }
    // Rehashing invalidates the first lookup's slot index. Otherwise reuse it
    // and avoid walking the same probe chain twice. Locate the slot before
    // appending because key may refer into keys_, whose reallocation would
    // invalidate the caller's span.
    if (old_capacity != slots_.size()) slot_index = find(hash, key, true);
    const std::size_t offset = keys_.size();
    try {
        keys_.insert(keys_.end(), key.begin(), key.end());
    } catch (const std::bad_alloc&) {
        ++stats_.rejected_capacity;
        stats_.saturated = true;
        return false;
    }
    slots_[slot_index] = Slot{hash, offset, threshold, true};
    ++size_;
    ++stats_.inserts;
    refresh_stats();
    return true;
}

void DynamicDecisionCache::refresh_stats() noexcept {
    stats_.entries = size_;
    stats_.capacity = slots_.size();
    stats_.memory_bytes = slots_.capacity() * sizeof(Slot) +
                          keys_.capacity() * sizeof(std::uint64_t);
    stats_.bytes_per_state = size_ == 0 ? 0.0 :
        static_cast<double>(stats_.memory_bytes) / static_cast<double>(size_);
}

void DynamicDecisionCache::clear() {
    size_ = 0;
    stats_ = {};
    slots_.clear();
    keys_.clear();
    (void)rehash(initial_capacity);
}

ShardedWord64DecisionCache::ShardedWord64DecisionCache(
    std::size_t shard_count, DecisionCacheOptions options) {
    if (shard_count == 0) throw std::invalid_argument("sharded cache requires at least one shard");
    shards_.reserve(shard_count);
    for (std::size_t index = 0; index < shard_count; ++index) {
        DecisionCacheOptions local = options;
        if (local.max_entries != 0)
            local.max_entries = std::max<std::size_t>(1, local.max_entries / shard_count);
        if (local.max_memory_bytes != 0)
            local.max_memory_bytes = std::max<std::size_t>(1, local.max_memory_bytes / shard_count);
        shards_.push_back(std::make_unique<Shard>(local));
    }
}

std::size_t ShardedWord64DecisionCache::shard_for(std::uint64_t key) const noexcept {
    // Use high hash bits for shard ownership; the flat table uses low bits for
    // its bucket, so this avoids correlating every shard with a small subset
    // of its internal buckets.
    return static_cast<std::size_t>(mix(key) >> 32U) % shards_.size();
}

bool ShardedWord64DecisionCache::proves_failed(std::uint64_t key,
                                                std::uint32_t threshold) {
    auto& shard = *shards_[shard_for(key)];
    std::lock_guard lock(shard.mutex);
    return shard.cache.proves_failed(key, threshold);
}

bool ShardedWord64DecisionCache::record_failed(std::uint64_t key,
                                                std::uint32_t threshold) {
    auto& shard = *shards_[shard_for(key)];
    std::lock_guard lock(shard.mutex);
    return shard.cache.record_failed(key, threshold);
}

DecisionCacheStats ShardedWord64DecisionCache::stats() const {
    DecisionCacheStats total;
    for (const auto& owned : shards_) {
        std::lock_guard lock(owned->mutex);
        const auto& part = owned->cache.stats();
        total.queries += part.queries;
        total.hits += part.hits;
        total.inserts += part.inserts;
        total.strengthenings += part.strengthenings;
        total.rejected_capacity += part.rejected_capacity;
        total.collisions += part.collisions;
        total.rehashes += part.rehashes;
        total.entries += part.entries;
        total.capacity += part.capacity;
        total.memory_bytes += part.memory_bytes;
    }
    return total;
}

ShardedFixedThresholdWord64Cache::ShardedFixedThresholdWord64Cache(
    std::size_t shard_count, std::uint32_t threshold, DecisionCacheOptions options) {
    if (shard_count == 0) throw std::invalid_argument("sharded cache requires at least one shard");
    shards_.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
        auto local = options;
        if (local.max_entries != 0)
            local.max_entries = std::max<std::size_t>(1, local.max_entries / shard_count);
        if (local.max_memory_bytes != 0)
            local.max_memory_bytes = std::max<std::size_t>(1, local.max_memory_bytes / shard_count);
        shards_.push_back(std::make_unique<Shard>(threshold, local));
    }
}

std::size_t ShardedFixedThresholdWord64Cache::shard_for(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>(mix(key) >> 32U) % shards_.size();
}

bool ShardedFixedThresholdWord64Cache::proves_failed(std::uint64_t key,
                                                      std::uint32_t threshold) {
    auto& shard = *shards_[shard_for(key)];
    std::lock_guard lock(shard.mutex);
    return shard.cache.proves_failed(key, threshold);
}

bool ShardedFixedThresholdWord64Cache::record_failed(std::uint64_t key,
                                                      std::uint32_t threshold) {
    auto& shard = *shards_[shard_for(key)];
    std::lock_guard lock(shard.mutex);
    return shard.cache.record_failed(key, threshold);
}

DecisionCacheStats ShardedFixedThresholdWord64Cache::stats() const {
    DecisionCacheStats total;
    for (const auto& owned : shards_) {
        std::lock_guard lock(owned->mutex);
        const auto& part = owned->cache.stats();
        total.queries += part.queries;
        total.hits += part.hits;
        total.inserts += part.inserts;
        total.rejected_capacity += part.rejected_capacity;
        total.collisions += part.collisions;
        total.rehashes += part.rehashes;
        total.entries += part.entries;
        total.capacity += part.capacity;
        total.memory_bytes += part.memory_bytes;
        total.saturated = total.saturated || part.saturated;
    }
    total.bytes_per_state = total.entries == 0 ? 0.0 :
        static_cast<double>(total.memory_bytes) / static_cast<double>(total.entries);
    return total;
}

ShardedDynamicDecisionCache::ShardedDynamicDecisionCache(
    std::size_t word_count, std::size_t shard_count,
    DecisionCacheOptions options)
    : word_count_(word_count) {
    if (word_count == 0)
        throw std::invalid_argument("sharded dynamic cache requires a nonzero word count");
    if (shard_count == 0)
        throw std::invalid_argument("sharded cache requires at least one shard");
    std::size_t effective_shards = shard_count;
    if (options.max_entries != 0)
        effective_shards = std::min(effective_shards, options.max_entries);
    if (options.max_memory_bytes != 0) {
        const auto footprint = DynamicDecisionCache::minimum_table_memory_bytes();
        effective_shards = std::min(effective_shards,
            std::max<std::size_t>(1, options.max_memory_bytes / footprint));
    }
    effective_shards = std::max<std::size_t>(1, effective_shards);
    shards_.reserve(effective_shards);
    for (std::size_t index = 0; index < effective_shards; ++index) {
        auto local = options;
        if (local.max_entries != 0) {
            const auto base = local.max_entries / effective_shards;
            const auto remainder = local.max_entries % effective_shards;
            local.max_entries = base + (index < remainder ? 1U : 0U);
        }
        if (local.max_memory_bytes != 0) {
            const auto base = local.max_memory_bytes / effective_shards;
            const auto remainder = local.max_memory_bytes % effective_shards;
            local.max_memory_bytes = base + (index < remainder ? 1U : 0U);
        }
        shards_.push_back(std::make_unique<Shard>(word_count, local));
    }
}

std::size_t ShardedDynamicDecisionCache::shard_for(
    std::uint64_t hash) const noexcept {
    return static_cast<std::size_t>(hash >> 32U) % shards_.size();
}

bool ShardedDynamicDecisionCache::proves_failed(
    std::span<const std::uint64_t> key, std::uint32_t threshold) {
    const auto hash = hash_words(key);
    auto& shard = *shards_[shard_for(hash)];
    std::lock_guard lock(shard.mutex);
    return shard.cache.proves_failed_hashed(key, threshold, hash);
}

bool ShardedDynamicDecisionCache::record_failed(
    std::span<const std::uint64_t> key, std::uint32_t threshold) {
    const auto hash = hash_words(key);
    auto& shard = *shards_[shard_for(hash)];
    std::lock_guard lock(shard.mutex);
    return shard.cache.record_failed_hashed(key, threshold, hash);
}

DecisionCacheStats ShardedDynamicDecisionCache::stats() const {
    DecisionCacheStats total;
    for (const auto& owned : shards_) {
        std::lock_guard lock(owned->mutex);
        const auto& part = owned->cache.stats();
        total.queries += part.queries;
        total.hits += part.hits;
        total.inserts += part.inserts;
        total.strengthenings += part.strengthenings;
        total.rejected_capacity += part.rejected_capacity;
        total.collisions += part.collisions;
        total.rehashes += part.rehashes;
        total.entries += part.entries;
        total.capacity += part.capacity;
        total.memory_bytes += part.memory_bytes;
        total.saturated = total.saturated || part.saturated;
    }
    total.bytes_per_state = total.entries == 0 ? 0.0 :
        static_cast<double>(total.memory_bytes) /
        static_cast<double>(total.entries);
    return total;
}

} // namespace cutwidth
