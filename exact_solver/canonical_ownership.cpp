#include "canonical_ownership.hpp"
#include "vertex_set.hpp"

#include <algorithm>
#include <stdexcept>

namespace cutwidth {

CanonicalOwnershipTable::CanonicalOwnershipTable(
    std::size_t words, std::size_t shard_count, std::size_t max_entries)
    : word_count_(words) {
    if (words == 0 || shard_count == 0)
        throw std::invalid_argument("canonical ownership dimensions must be nonzero");
    if (max_entries != 0) shard_count = std::min(shard_count, max_entries);
    shard_count = std::max<std::size_t>(1, shard_count);
    max_entries_per_shard_ = max_entries == 0 ? 0 :
        std::max<std::size_t>(1, (max_entries + shard_count - 1) / shard_count);
    shards_.reserve(shard_count);
    for (std::size_t i=0;i<shard_count;++i) shards_.push_back(std::make_unique<Shard>());
}

std::size_t CanonicalOwnershipTable::shard_for(std::uint64_t hash) const noexcept {
    return static_cast<std::size_t>(hash >> 32U) % shards_.size();
}

OwnershipAcquire CanonicalOwnershipTable::acquire(
    std::span<const std::uint64_t> key, std::uint64_t owner) {
    if (key.size()!=word_count_ || owner==0)
        throw std::invalid_argument("invalid canonical ownership claim");
    const auto hash=hash_words(key);auto& shard=*shards_[shard_for(hash)];
    std::lock_guard lock(shard.mutex);
    for(auto& entry:shard.entries){
        if(entry.hash!=hash || !std::equal(key.begin(),key.end(),entry.key.begin()))continue;
        if(entry.owner==owner)return OwnershipAcquire::acquired;
        if (std::find(entry.waiters.begin(), entry.waiters.end(), owner) ==
            entry.waiters.end())
            entry.waiters.push_back(owner);
        {std::lock_guard stats_lock(stats_mutex_);++stats_.duplicate_waits;}
        return OwnershipAcquire::duplicate;
    }
    if(max_entries_per_shard_!=0 && shard.entries.size()>=max_entries_per_shard_){
        std::lock_guard stats_lock(stats_mutex_);++stats_.saturated;return OwnershipAcquire::saturated;
    }
    try{shard.entries.push_back({hash,{key.begin(),key.end()},owner,{}});}
    catch(const std::bad_alloc&){std::lock_guard stats_lock(stats_mutex_);++stats_.saturated;
        return OwnershipAcquire::saturated;}
    {std::lock_guard stats_lock(stats_mutex_);++stats_.acquired;++stats_.entries;}
    return OwnershipAcquire::acquired;
}

void CanonicalOwnershipTable::publish_failure(
    std::span<const std::uint64_t> key, std::uint64_t owner) {
    if(key.size()!=word_count_||owner==0)throw std::invalid_argument("invalid ownership publication");
    release(key,owner,true);
}

void CanonicalOwnershipTable::abandon(
    std::span<const std::uint64_t> key, std::uint64_t owner) noexcept {
    if (key.size() != word_count_ || owner == 0) return;
    release(key, owner, false);
}

void CanonicalOwnershipTable::release(
    std::span<const std::uint64_t> key, std::uint64_t owner, bool failed) noexcept {
    const auto hash = hash_words(key);
    auto& shard = *shards_[shard_for(hash)];
    std::vector<std::uint64_t> wake;
    bool found = false;
    {
        std::lock_guard lock(shard.mutex);
        for (auto it = shard.entries.begin(); it != shard.entries.end(); ++it) {
            if (it->hash == hash && it->owner == owner &&
                std::equal(key.begin(), key.end(), it->key.begin())) {
                wake = std::move(it->waiters);
                shard.entries.erase(it);
                found = true;
                break;
            }
        }
    }
    if (!found) return;
    if (!wake.empty()) {
        std::lock_guard lock(ready_mutex_);
        ready_waiters_.insert(ready_waiters_.end(), wake.begin(), wake.end());
    }
    {
        std::lock_guard stats_lock(stats_mutex_);
        if (failed) ++stats_.failure_publications;
        else ++stats_.abandoned;
        stats_.waiters_woken += wake.size();
        if (stats_.entries != 0) --stats_.entries;
    }
}

std::vector<std::uint64_t> CanonicalOwnershipTable::take_ready_waiters(){
    std::lock_guard lock(ready_mutex_);auto result=std::move(ready_waiters_);ready_waiters_.clear();return result;
}
CanonicalOwnershipStats CanonicalOwnershipTable::stats() const{
    std::lock_guard lock(stats_mutex_);return stats_;
}

} // namespace cutwidth
