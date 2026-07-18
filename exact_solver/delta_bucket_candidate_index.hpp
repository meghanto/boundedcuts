#pragma once

#include "graph.hpp"
#include "vertex_set.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace cutwidth {

// Reversible derived state for candidate discovery. The proof state remains
// the prefix/path plus per-frame sorted candidates; snapshots reconstruct this
// index deterministically instead of serializing bucket-internal order.
class DeltaBucketCandidateIndex {
public:
    DeltaBucketCandidateIndex(const Graph& graph, const VertexSet& prefix,
                              const std::vector<std::uint32_t>& before)
        : maximum_degree_(0), delta_(graph.size()), bucket_(graph.size()),
          position_(graph.size()), active_(graph.size(), false) {
        for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex)
            maximum_degree_ = std::max(maximum_degree_, graph.degree(vertex));
        buckets_.resize(2U * maximum_degree_ + 1U);
        for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex) {
            if (prefix.contains(vertex)) continue;
            insert(vertex, static_cast<std::int32_t>(graph.degree(vertex)) -
                2 * static_cast<std::int32_t>(before[vertex]));
        }
    }

    [[nodiscard]] bool active(Graph::Vertex vertex) const noexcept {
        return active_[vertex];
    }

    [[nodiscard]] std::int32_t delta(Graph::Vertex vertex) const noexcept {
        return delta_[vertex];
    }

    void remove(Graph::Vertex vertex) {
        if (!active_[vertex]) throw std::logic_error("candidate index double removal");
        auto& values = buckets_[bucket_[vertex]];
        const auto position = position_[vertex];
        const auto moved = values.back();
        values[position] = moved;
        position_[moved] = position;
        values.pop_back();
        active_[vertex] = false;
    }

    void insert(Graph::Vertex vertex, std::int32_t delta) {
        if (active_[vertex]) throw std::logic_error("candidate index double insertion");
        const auto index = bucket_index(delta);
        delta_[vertex] = delta;
        bucket_[vertex] = index;
        position_[vertex] = buckets_[index].size();
        buckets_[index].push_back(vertex);
        active_[vertex] = true;
    }

    void move(Graph::Vertex vertex, std::int32_t delta) {
        remove(vertex);
        insert(vertex, delta);
    }

    template <typename Emit>
    std::size_t gather(std::int32_t maximum_delta, Emit emit) const {
        const auto upper = std::min(maximum_delta,
            static_cast<std::int32_t>(maximum_degree_));
        if (upper < -static_cast<std::int32_t>(maximum_degree_)) return 0;
        std::size_t slots = 0;
        for (std::int32_t delta = -static_cast<std::int32_t>(maximum_degree_);
             delta <= upper; ++delta) {
            ++slots;
            for (const auto vertex : buckets_[bucket_index(delta)])
                emit(vertex, delta);
        }
        return slots;
    }

    void validate(const Graph& graph, const VertexSet& prefix,
                  const std::vector<std::uint32_t>& before) const {
        std::vector<bool> seen(graph.size(), false);
        for (std::size_t index = 0; index < buckets_.size(); ++index) {
            for (std::size_t position = 0; position < buckets_[index].size(); ++position) {
                const auto vertex = buckets_[index][position];
                if (vertex >= graph.size() || seen[vertex] || !active_[vertex] ||
                    bucket_[vertex] != index || position_[vertex] != position)
                    throw std::logic_error("candidate index membership corruption");
                seen[vertex] = true;
            }
        }
        for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex) {
            const bool expected_active = !prefix.contains(vertex);
            if (active_[vertex] != expected_active || seen[vertex] != expected_active)
                throw std::logic_error("candidate index placed-state mismatch");
            if (!expected_active) continue;
            const auto expected_delta = static_cast<std::int32_t>(graph.degree(vertex)) -
                2 * static_cast<std::int32_t>(before[vertex]);
            if (delta_[vertex] != expected_delta ||
                bucket_[vertex] != bucket_index(expected_delta))
                throw std::logic_error("candidate index delta mismatch");
        }
    }

private:
    [[nodiscard]] std::size_t bucket_index(std::int32_t delta) const {
        const auto shifted = delta + static_cast<std::int32_t>(maximum_degree_);
        if (shifted < 0 || shifted >= static_cast<std::int32_t>(buckets_.size()))
            throw std::logic_error("candidate delta outside graph degree range");
        return static_cast<std::size_t>(shifted);
    }

    std::uint32_t maximum_degree_ = 0;
    std::vector<std::int32_t> delta_;
    std::vector<std::size_t> bucket_;
    std::vector<std::size_t> position_;
    std::vector<bool> active_;
    std::vector<std::vector<Graph::Vertex>> buckets_;
};

} // namespace cutwidth
