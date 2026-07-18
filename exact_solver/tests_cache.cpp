#include "decision.hpp"
#include "decision_cache.hpp"
#include "canonical_ownership.hpp"
#include "graph.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void table_tests() {
    cutwidth::DecisionCacheOptions limited;
    limited.max_entries = 1;
    cutwidth::Word64DecisionCache small(limited);
    require(small.record_failed(0, 5), "failed to record key zero");
    require(small.proves_failed(0, 4) && small.proves_failed(0, 5), "monotone proof missing");
    require(!small.proves_failed(0, 6), "smaller-threshold proof pruned larger threshold");
    require(small.record_failed(0, 7) && small.proves_failed(0, 7), "full cache did not strengthen");
    require(!small.record_failed(1, 1), "entry limit was ignored");

    cutwidth::FixedThresholdWord64Cache fixed(5);
    require(fixed.record_failed(0, 5), "fixed cache failed to record key zero");
    require(fixed.proves_failed(0, 5), "fixed cache lost its proof");
    require(!fixed.proves_failed(0, 4) && !fixed.proves_failed(0, 6),
            "fixed cache crossed threshold boundaries");
    require(!fixed.record_failed(1, 4), "fixed cache accepted a foreign threshold");
    require(fixed.stats().memory_bytes == fixed.stats().capacity * 9,
            "fixed cache storage is not compact");

    cutwidth::DynamicDecisionCache dynamic(2);
    const std::array<std::uint64_t, 2> a{0, 0};
    const std::array<std::uint64_t, 2> b{1, 0x8000000000000000ULL};
    require(dynamic.record_failed(a, 2) && dynamic.record_failed(b, 9), "dynamic insertion failed");
    require(dynamic.proves_failed(a, 2) && !dynamic.proves_failed(a, 3), "dynamic threshold semantics wrong");
    require(dynamic.proves_failed(b, 8), "dynamic full-key lookup failed");

    cutwidth::FixedThresholdDynamicCache compact(4, 7);
    cutwidth::DynamicDecisionCache cross_threshold(4);
    for (std::uint64_t key = 0; key < 1000; ++key) {
        const std::array<std::uint64_t, 4> words{
            key, key * 3, ~key, key * 0x9e3779b97f4a7c15ULL};
        require(compact.record_failed(words, 7), "compact dynamic insertion failed");
        require(cross_threshold.record_failed(words, 7), "comparison cache insertion failed");
    }
    const std::array<std::uint64_t, 4> retained{999, 999 * 3, ~std::uint64_t{999},
        999 * 0x9e3779b97f4a7c15ULL};
    require(compact.proves_failed(retained, 7), "compact dynamic full-key hit failed");
    const std::array<std::uint64_t, 4> earliest{0, 0, ~std::uint64_t{0}, 0};
    require(compact.proves_failed(earliest, 7) &&
            compact.stats().segment_growths != 0 && compact.stats().rehashes == 0,
            "geometric segment growth rebuilt or lost an older proof page");
    require(compact.stats().bytes_per_state <=
                0.75 * cross_threshold.stats().bytes_per_state,
            "compact dynamic cache missed the 25 percent layout gate");

    cutwidth::DecisionCacheOptions saturated_options;
    saturated_options.max_entries = 1;
    saturated_options.max_memory_bytes = 0;
    cutwidth::FixedThresholdDynamicCache saturated(4, 3, saturated_options);
    const std::array<std::uint64_t, 4> first_key{1, 2, 3, 4};
    const std::array<std::uint64_t, 4> rejected_key{5, 6, 7, 8};
    require(saturated.record_failed(first_key, 3), "bounded compact cache lost first key");
    require(!saturated.record_failed(rejected_key, 3), "bounded compact cache exceeded cap");
    const auto probes = saturated.stats().insertion_probes;
    require(!saturated.record_failed(rejected_key, 3) &&
            saturated.stats().insertion_probes == probes &&
            saturated.stats().probes_avoided_after_saturation >= 2,
            "saturated compact cache still walked insertion probes");
    require(saturated.proves_failed(first_key, 3),
            "saturated compact cache stopped serving retained hits");

    // New insertions must walk a colliding probe chain only once unless the
    // insertion triggers a rehash. These keys all occupy the same initial
    // bucket because the table starts with 16 slots.
    cutwidth::Word64DecisionCache collision_cache;
    std::vector<std::uint64_t> colliding;
    for (std::uint64_t key = 0; colliding.size() < 3; ++key) {
        cutwidth::Word64DecisionCache probe;
        probe.record_failed(key, 1);
        // Find colliders without depending on the private hash function by
        // observing collision counts against the first selected key.
        if (colliding.empty()) {
            colliding.push_back(key);
        } else {
            cutwidth::Word64DecisionCache candidate;
            candidate.record_failed(colliding.front(), 1);
            const auto before = candidate.stats().collisions;
            candidate.record_failed(key, 1);
            if (candidate.stats().collisions > before) colliding.push_back(key);
        }
    }
    require(collision_cache.record_failed(colliding[0], 1), "collider insert failed");
    require(collision_cache.record_failed(colliding[1], 1), "collider insert failed");
    const auto before_collision_insert = collision_cache.stats().collisions;
    require(collision_cache.record_failed(colliding[2], 1), "collider insert failed");
    require(collision_cache.stats().collisions - before_collision_insert == 2,
            "new insertion probed its collision chain more than once");
}

void canonical_ownership_test() {
    using cutwidth::OwnershipAcquire;
    cutwidth::CanonicalOwnershipTable ownership(2, 1, 2);
    const std::array<std::uint64_t, 2> a{1, 7};
    const std::array<std::uint64_t, 2> b{1, 8};
    const std::array<std::uint64_t, 2> c{2, 9};

    require(ownership.acquire(a, 1) == OwnershipAcquire::acquired,
            "canonical owner failed to claim a key");
    require(ownership.acquire(b, 2) == OwnershipAcquire::acquired,
            "ownership routing confused distinct full keys");
    require(ownership.acquire(a, 3) == OwnershipAcquire::duplicate,
            "duplicate canonical state was not reported");
    require(ownership.acquire(c, 4) == OwnershipAcquire::saturated,
            "bounded ownership table exceeded capacity");
    ownership.publish_failure(a, 1);
    auto ready = ownership.take_ready_waiters();
    require(ready.size() == 1 && ready.front() == 3,
            "published owner did not wake its exact duplicate");

    ownership.abandon(b, 2);
    require(ownership.acquire(c, 4) == OwnershipAcquire::acquired,
            "abandonment did not release ownership capacity");
    ownership.abandon(c, 4);
    const auto stats = ownership.stats();
    require(stats.entries == 0 && stats.acquired == 3 &&
                stats.duplicate_waits == 1 && stats.saturated == 1 &&
                stats.failure_publications == 1 && stats.abandoned == 2 &&
                stats.waiters_woken == 1,
            "canonical ownership statistics are inconsistent");
}

void shared_threshold_test() {
    cutwidth::Graph path(6, {{0,1},{1,2},{2,3},{3,4},{4,5}});
    cutwidth::DecisionCacheOptions config;
    cutwidth::Word64DecisionCache cache(config);
    cutwidth::DecisionOptions options;
    const auto no = cutwidth::decide_cutwidth_cached(path, 0, options, cache);
    require(no.status == cutwidth::DecisionStatus::infeasible, "P6 accepted at width zero");
    const auto cached_no = cutwidth::decide_cutwidth_cached(path, 0, options, cache);
    require(cached_no.status == cutwidth::DecisionStatus::infeasible &&
            cached_no.stats.failed_cache_hits > 0, "cross-call proof was not reused");
    const auto yes = cutwidth::decide_cutwidth_cached(path, 1, options, cache);
    require(yes.status == cutwidth::DecisionStatus::feasible &&
            path.ordering_cutwidth(yes.ordering) == 1,
            "lower-threshold proof incorrectly pruned a larger threshold");
}

void sharded_concurrency_test() {
    cutwidth::ShardedWord64DecisionCache cache(16);
    std::vector<std::thread> workers;
    for (std::uint32_t worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&cache, worker] {
            for (std::uint64_t key = 0; key < 2000; ++key)
                require(cache.record_failed(key, worker + 1),
                        "concurrent sharded insertion failed");
        });
    }
    for (auto& worker : workers) worker.join();
    for (std::uint64_t key = 0; key < 2000; ++key) {
        require(cache.proves_failed(key, 8), "concurrent strengthening was lost");
        require(!cache.proves_failed(key, 9), "sharded cache violated max-failed semantics");
    }
    const auto stats = cache.stats();
    require(stats.entries == 2000 && stats.inserts == 2000,
            "sharded cache aggregate statistics are wrong");

    cutwidth::ShardedFixedThresholdWord64Cache fixed(16, 8);
    workers.clear();
    for (std::uint32_t worker = 0; worker < 8; ++worker)
        workers.emplace_back([&fixed, worker] {
            for (std::uint64_t key = worker; key < 2000; key += 8)
                require(fixed.record_failed(key, 8), "fixed shard insertion failed");
        });
    for (auto& worker : workers) worker.join();
    require(fixed.stats().entries == 2000, "fixed sharded cache lost entries");
    require(fixed.proves_failed(1999, 8) && !fixed.proves_failed(1999, 7),
            "fixed sharded cache crossed thresholds");

    cutwidth::ShardedDynamicDecisionCache dynamic(2, 16);
    workers.clear();
    for (std::uint32_t worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&dynamic, worker] {
            for (std::uint64_t key = 0; key < 2000; ++key) {
                const std::array<std::uint64_t, 2> words{
                    key, key * 0x9e3779b97f4a7c15ULL};
                require(dynamic.record_failed(words, worker + 1),
                        "concurrent dynamic insertion failed");
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (std::uint64_t key = 0; key < 2000; ++key) {
        const std::array<std::uint64_t, 2> words{
            key, key * 0x9e3779b97f4a7c15ULL};
        require(dynamic.proves_failed(words, 8),
                "concurrent dynamic strengthening was lost");
        require(!dynamic.proves_failed(words, 9),
                "sharded dynamic cache violated max-failed semantics");
    }
    require(dynamic.stats().entries == 2000 && dynamic.stats().inserts == 2000,
            "sharded dynamic aggregate statistics are wrong");

    cutwidth::ShardedFixedThresholdDynamicCache fixed_dynamic(4, 16, 11);
    workers.clear();
    for (std::uint32_t worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&fixed_dynamic, worker] {
            for (std::uint64_t key = worker; key < 2000; key += 8) {
                const std::array<std::uint64_t, 4> words{
                    key, key * 3, ~key, key * 0x9e3779b97f4a7c15ULL};
                require(fixed_dynamic.record_failed(words, 11),
                        "shared fixed dynamic insertion failed");
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (std::uint64_t key = 0; key < 2000; ++key) {
        const std::array<std::uint64_t, 4> words{
            key, key * 3, ~key, key * 0x9e3779b97f4a7c15ULL};
        require(fixed_dynamic.proves_failed(words, 11),
                "shared fixed dynamic cache lost full-key proof");
    }
    require(fixed_dynamic.stats().entries == 2000 &&
            fixed_dynamic.stats().inserts == 2000,
            "shared fixed dynamic aggregate statistics are wrong");

    cutwidth::DecisionCacheOptions tiny;
    tiny.max_entries = 3;
    tiny.max_memory_bytes =
        cutwidth::DynamicDecisionCache::minimum_table_memory_bytes();
    cutwidth::ShardedDynamicDecisionCache bounded(2, 64, tiny);
    require(bounded.shard_count() == 1,
            "tiny dynamic budget created unbounded empty shards");
    for (std::uint64_t key = 0; key < 20; ++key) {
        const std::array<std::uint64_t, 2> words{key, ~key};
        (void)bounded.record_failed(words, 1);
    }
    require(bounded.stats().entries <= tiny.max_entries,
            "sharded dynamic cache exceeded its global entry budget");
    require(bounded.stats().saturated,
            "sharded dynamic cache did not report saturation");

    cutwidth::DecisionCacheOptions entry_limited;
    entry_limited.max_entries = 3;
    entry_limited.max_memory_bytes = 0;
    cutwidth::ShardedDynamicDecisionCache only_three(2, 64, entry_limited);
    for (std::uint64_t key = 0; key < 20; ++key) {
        const std::array<std::uint64_t, 2> words{key, key + 1};
        (void)only_three.record_failed(words, 2);
    }
    require(only_three.stats().entries <= 3 && only_three.stats().saturated,
            "sharded dynamic cache exceeded its divided entry cap");
}

void fixed_dynamic_snapshot_roundtrip_test() {
    cutwidth::DecisionCacheOptions bounded;
    bounded.max_entries = 96;
    bounded.max_memory_bytes = 1U << 20;
    cutwidth::ShardedFixedThresholdDynamicCache original(3, 4, 11, bounded);
    for (std::uint64_t key = 0; key < 256; ++key) {
        const std::array<std::uint64_t, 3> words{
            key, key * 0x9e3779b97f4a7c15ULL, ~key};
        (void)original.record_failed(words, 11);
    }
    require(original.stats().saturated, "bounded fixed dynamic cache did not saturate");
    const auto image = original.snapshot();
    require(image.shard_count == 4 && image.shards.size() == 4,
            "fixed dynamic snapshot lost shard topology");
    auto restored = cutwidth::ShardedFixedThresholdDynamicCache::restore(image);
    const auto restored_image = restored.snapshot();
    require(restored_image.word_count == image.word_count &&
            restored_image.threshold == image.threshold &&
            restored_image.shard_count == image.shard_count,
            "fixed dynamic snapshot changed cache identity");
    for (std::size_t shard = 0; shard < image.shards.size(); ++shard) {
        const auto& before = image.shards[shard];
        const auto& after = restored_image.shards[shard];
        require(after.size == before.size &&
                after.stats.entries == before.stats.entries &&
                after.stats.capacity == before.stats.capacity &&
                after.stats.memory_bytes == before.stats.memory_bytes &&
                after.stats.queries == before.stats.queries &&
                after.stats.hits == before.stats.hits &&
                after.stats.inserts == before.stats.inserts &&
                after.stats.rejected_capacity == before.stats.rejected_capacity &&
                after.stats.collisions == before.stats.collisions &&
                after.stats.lookup_probes == before.stats.lookup_probes &&
                after.stats.insertion_probes == before.stats.insertion_probes &&
                after.stats.probes_avoided_after_saturation ==
                    before.stats.probes_avoided_after_saturation &&
                after.stats.saturated == before.stats.saturated,
                "fixed dynamic snapshot changed accounting");
        require(after.segments.size() == before.segments.size(),
                "fixed dynamic snapshot changed segment count");
        for (std::size_t segment = 0; segment < before.segments.size(); ++segment) {
            const auto& lhs = before.segments[segment];
            const auto& rhs = after.segments[segment];
            require(lhs.control == rhs.control && lhs.dense_index == rhs.dense_index &&
                    lhs.keys == rhs.keys && lhs.bloom == rhs.bloom &&
                    lhs.size == rhs.size && lhs.control_capacity == rhs.control_capacity &&
                    lhs.dense_index_capacity == rhs.dense_index_capacity &&
                    lhs.keys_capacity == rhs.keys_capacity &&
                    lhs.bloom_capacity == rhs.bloom_capacity,
                    "fixed dynamic snapshot changed physical segment layout");
        }
    }
    for (std::uint64_t key = 0; key < 256; ++key) {
        const std::array<std::uint64_t, 3> words{
            key, key * 0x9e3779b97f4a7c15ULL, ~key};
        if (original.proves_failed(words, 11))
            require(restored.proves_failed(words, 11),
                    "restored fixed dynamic cache lost a retained proof");
    }
    const std::array<std::uint64_t, 3> fresh{1000003, 7, 9};
    require(!restored.record_failed(fresh, 11),
            "restored saturated fixed dynamic cache admitted a fresh proof");
}

void generational_page_clock_test() {
    cutwidth::DecisionCacheOptions options;
    options.max_entries = 22;
    options.max_memory_bytes = 0;
    options.replacement = cutwidth::CacheReplacementPolicy::generational_clock;
    options.replacement_page_capacity = 16;
    cutwidth::FixedThresholdDynamicCache cache(1, 9, options);

    std::vector<std::array<std::uint64_t, 1>> shallow;
    std::vector<std::array<std::uint64_t, 1>> deep;
    for (std::uint32_t bit = 0; bit < 11; ++bit) {
        shallow.push_back({std::uint64_t{1} << bit});
        deep.push_back({~(std::uint64_t{1} << bit)});
        require(cache.record_failed(shallow.back(), 9),
                "generational cache lost a shallow insertion");
    }
    for (const auto& key : deep)
        require(cache.record_failed(key, 9),
                "generational cache lost a deep insertion");
    require(cache.stats().entries == 22,
            "generational fixture did not fill both pages");

    const std::array<std::uint64_t, 1> fresh{0x5555555555555555ULL};
    require(!cache.record_failed(fresh, 9),
            "first CLOCK sweep should demote rather than immediately evict");
    require(cache.proves_failed(deep.front(), 9),
            "hot-page fixture could not promote its retained deep page");
    require(cache.record_failed(fresh, 9),
            "generational cache rejected the post-demotion admission");
    const auto stats = cache.stats();
    require(stats.saturated && stats.pages_recycled == 1 &&
            stats.replacement_admissions == 1 && stats.entries_evicted == 11,
            "generational cache replacement accounting is wrong");
    require(stats.page_second_chances >= 2 && stats.page_promotions >= 1 &&
            stats.maximum_evicted_depth == 1,
            "CLOCK did not prefer the shallow cold page");
    require(cache.proves_failed(fresh, 9) && cache.proves_failed(deep.front(), 9),
            "generational cache lost the fresh or deep retained proof");
    require(!cache.proves_failed(shallow.front(), 9),
            "generational cache failed to evict the selected shallow page");

    const auto image = cache.snapshot();
    auto restored = cutwidth::FixedThresholdDynamicCache::restore(image);
    const auto restored_image = restored.snapshot();
    require(restored_image.clock_hand == image.clock_hand &&
            restored_image.stats.pages_recycled == image.stats.pages_recycled &&
            restored_image.options.replacement == image.options.replacement,
            "generational snapshot lost CLOCK state");
    require(restored_image.segments.size() == image.segments.size(),
            "generational snapshot changed page count");
    for (std::size_t index = 0; index < image.segments.size(); ++index) {
        require(restored_image.segments[index].referenced ==
                    image.segments[index].referenced &&
                restored_image.segments[index].depth_sum ==
                    image.segments[index].depth_sum &&
                restored_image.segments[index].maximum_depth ==
                    image.segments[index].maximum_depth,
                "generational snapshot changed page metadata");
    }
}
}

int main() {
    try {
        table_tests();
        canonical_ownership_test();
        shared_threshold_test();
        sharded_concurrency_test();
        fixed_dynamic_snapshot_roundtrip_test();
        generational_page_clock_test();
        std::cout << "All decision-cache tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CACHE TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
