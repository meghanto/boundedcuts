#pragma once

#include "graph.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace cutwidth {

enum class NodeMemoProof : std::uint8_t { none, prefix_cut, finite_horizon };

struct NodeMemoValue {
    Graph::Mask prefix = 0;
    std::uint32_t bound = 0;
    std::uint8_t completed_depth = 0;
    NodeMemoProof proof = NodeMemoProof::none;
    bool complete = false;
};

struct NodeMemoStats {
    std::array<std::uint64_t, 5> hits_by_depth{};
    std::uint64_t computations = 0;
    std::uint64_t collisions = 0;
    std::uint64_t saturation = 0;
    std::size_t entries = 0;
    std::size_t capacity = 0;
    std::size_t memory_bytes = 0;
};

// A fixed-capacity, overwrite-on-collision hint table. A slot is consumed only
// after exact prefix equality, so collisions can lose work but never transfer a
// certificate to a different state.
class NodeMemoTable {
public:
    explicit NodeMemoTable(std::size_t memory_bytes, std::size_t shards = 16);
    [[nodiscard]] std::optional<NodeMemoValue> find(Graph::Mask prefix,
                                                    std::uint8_t depth);
    void store(const NodeMemoValue& value);
    [[nodiscard]] NodeMemoStats stats() const;

private:
    struct Slot { NodeMemoValue value{}; bool occupied = false; };
    struct Shard {
        mutable std::mutex mutex;
        std::vector<Slot> slots;
        std::size_t entries = 0;
    };
    [[nodiscard]] static std::uint64_t hash(Graph::Mask key) noexcept;
    [[nodiscard]] std::size_t shard_for(Graph::Mask key) const noexcept;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::array<std::atomic<std::uint64_t>, 5> hits_{};
    std::atomic<std::uint64_t> computations_{0}, collisions_{0}, saturation_{0};
    std::size_t memory_bytes_ = 0;

    friend class FiniteHorizonOracle;
};

struct NodeOracleResult {
    std::uint32_t bound = 0;
    bool complete = false;
    std::uint8_t completed_depth = 0;
    NodeMemoProof proof = NodeMemoProof::none;
};

class FiniteHorizonOracle {
public:
    FiniteHorizonOracle(const Graph& graph, std::shared_ptr<NodeMemoTable> memo = {});
    [[nodiscard]] NodeOracleResult evaluate(
        Graph::Mask prefix, std::uint8_t depth,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        const std::atomic<bool>* stop = nullptr);

private:
    [[nodiscard]] bool interrupted(std::chrono::steady_clock::time_point deadline,
                                   const std::atomic<bool>* stop) const;
    const Graph& graph_;
    std::shared_ptr<NodeMemoTable> memo_;
    Graph::Mask all_ = 0;
};

} // namespace cutwidth
