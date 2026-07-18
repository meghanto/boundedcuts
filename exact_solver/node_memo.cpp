#include "node_memo.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace cutwidth {

std::uint64_t NodeMemoTable::hash(Graph::Mask key) noexcept {
    key ^= key >> 30; key *= 0xbf58476d1ce4e5b9ULL;
    key ^= key >> 27; key *= 0x94d049bb133111ebULL;
    return key ^ (key >> 31);
}

NodeMemoTable::NodeMemoTable(std::size_t memory_bytes, std::size_t shard_count)
    : memory_bytes_(memory_bytes) {
    if (shard_count == 0) throw std::invalid_argument("node memo shard count must be positive");
    const auto capacity = memory_bytes / sizeof(Slot);
    if (capacity == 0) return;
    shard_count = std::min(shard_count, capacity);
    shards_.reserve(shard_count);
    const auto base = capacity / shard_count;
    const auto extra = capacity % shard_count;
    for (std::size_t i = 0; i < shard_count; ++i) {
        auto shard = std::make_unique<Shard>();
        shard->slots.resize(base + (i < extra));
        shards_.push_back(std::move(shard));
    }
}

std::size_t NodeMemoTable::shard_for(Graph::Mask key) const noexcept {
    return shards_.empty() ? 0 : hash(key) % shards_.size();
}

std::optional<NodeMemoValue> NodeMemoTable::find(Graph::Mask prefix, std::uint8_t depth) {
    if (shards_.empty() || depth > 4) return std::nullopt;
    auto& shard = *shards_[shard_for(prefix)];
    std::lock_guard lock(shard.mutex);
    const auto& slot = shard.slots[hash(prefix) % shard.slots.size()];
    if (!slot.occupied) return std::nullopt;
    if (slot.value.prefix != prefix) {
        collisions_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    // H_d is depth-specific. A deeper value is a valid stronger lower bound,
    // but it is not the exact H_d requested by recurrence evaluation.
    if (!slot.value.complete || slot.value.completed_depth != depth) return std::nullopt;
    hits_[depth].fetch_add(1, std::memory_order_relaxed);
    return slot.value;
}

void NodeMemoTable::store(const NodeMemoValue& value) {
    if (shards_.empty()) { saturation_.fetch_add(1, std::memory_order_relaxed); return; }
    auto& shard = *shards_[shard_for(value.prefix)];
    std::lock_guard lock(shard.mutex);
    auto& slot = shard.slots[hash(value.prefix) % shard.slots.size()];
    if (slot.occupied && slot.value.prefix != value.prefix) {
        collisions_.fetch_add(1, std::memory_order_relaxed);
        saturation_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!slot.occupied) ++shard.entries;
    if (!slot.occupied || slot.value.prefix != value.prefix ||
        value.completed_depth >= slot.value.completed_depth ||
        value.bound > slot.value.bound) {
        slot.value = value;
        slot.occupied = true;
    }
}

NodeMemoStats NodeMemoTable::stats() const {
    NodeMemoStats result;
    for (std::size_t i = 0; i < result.hits_by_depth.size(); ++i)
        result.hits_by_depth[i] = hits_[i].load(std::memory_order_relaxed);
    result.computations = computations_.load(std::memory_order_relaxed);
    result.collisions = collisions_.load(std::memory_order_relaxed);
    result.saturation = saturation_.load(std::memory_order_relaxed);
    result.memory_bytes = memory_bytes_;
    for (const auto& shard : shards_) {
        std::lock_guard lock(shard->mutex);
        result.entries += shard->entries;
        result.capacity += shard->slots.size();
    }
    return result;
}

FiniteHorizonOracle::FiniteHorizonOracle(const Graph& graph,
                                         std::shared_ptr<NodeMemoTable> memo)
    : graph_(graph), memo_(std::move(memo)) {
    if (!graph.supports_mask())
        throw std::invalid_argument("node memo oracle requires at most 63 vertices");
    all_ = graph.size() == 0 ? 0 : (Graph::Mask{1} << graph.size()) - 1;
}

bool FiniteHorizonOracle::interrupted(
    std::chrono::steady_clock::time_point deadline, const std::atomic<bool>* stop) const {
    return (stop && stop->load(std::memory_order_relaxed)) ||
        (deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline);
}

NodeOracleResult FiniteHorizonOracle::evaluate(
    Graph::Mask prefix, std::uint8_t depth,
    std::chrono::steady_clock::time_point deadline, const std::atomic<bool>* stop) {
    if (depth > 4) throw std::invalid_argument("node memo depth must be between 0 and 4");
    if ((prefix & ~all_) != 0) throw std::invalid_argument("node memo prefix contains unknown vertex");
    if (memo_) {
        if (auto cached = memo_->find(prefix, depth))
            return {cached->bound, true, cached->completed_depth, cached->proof};
        memo_->computations_.fetch_add(1, std::memory_order_relaxed);
    }
    const auto delta = graph_.cut(prefix);
    if (depth == 0 || prefix == all_) {
        NodeOracleResult result{delta, true, depth, depth == 0 ? NodeMemoProof::prefix_cut
                                                               : NodeMemoProof::finite_horizon};
        if (memo_) memo_->store({prefix, result.bound, result.completed_depth, result.proof, true});
        return result;
    }
    if (interrupted(deadline, stop)) return {delta, false, 0, NodeMemoProof::prefix_cut};
    std::uint32_t minimum = std::numeric_limits<std::uint32_t>::max();
    auto remaining = all_ & ~prefix;
    while (remaining != 0) {
        if (interrupted(deadline, stop)) return {delta, false, 0, NodeMemoProof::prefix_cut};
        const auto v = static_cast<Graph::Vertex>(std::countr_zero(remaining));
        remaining &= remaining - 1;
        const auto child = evaluate(prefix | (Graph::Mask{1} << v), depth - 1,
                                    deadline, stop);
        if (!child.complete) return {delta, false, 0, NodeMemoProof::prefix_cut};
        minimum = std::min(minimum, child.bound);
    }
    const auto value = std::max(delta, minimum);
    if (memo_) memo_->store({prefix, value, depth, NodeMemoProof::finite_horizon, true});
    return {value, true, depth, NodeMemoProof::finite_horizon};
}

} // namespace cutwidth
