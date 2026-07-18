#pragma once

#include "graph.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cutwidth {

struct IncrementalLayoutStats {
    std::uint64_t interval_evaluations = 0;
    std::uint64_t full_fallbacks = 0;
};

class IncrementalLayoutEvaluator {
public:
    IncrementalLayoutEvaluator(const Graph& graph, std::vector<Graph::Vertex> ordering);
    [[nodiscard]] const std::vector<Graph::Vertex>& ordering() const noexcept { return order_; }
    [[nodiscard]] const std::vector<std::uint32_t>& cuts() const noexcept { return cuts_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return maximum_; }
    [[nodiscard]] std::vector<std::uint32_t> descending_cut_profile() const;

    [[nodiscard]] std::vector<std::uint32_t> cuts_after_swap(std::size_t a, std::size_t b);
    [[nodiscard]] std::vector<std::uint32_t> cuts_after_insertion(std::size_t from, std::size_t to);
    [[nodiscard]] std::vector<std::uint32_t> cuts_after_reversal(std::size_t left, std::size_t right);
    void apply_swap(std::size_t a, std::size_t b);
    void apply_insertion(std::size_t from, std::size_t to);
    void apply_reversal(std::size_t left, std::size_t right);
    [[nodiscard]] const IncrementalLayoutStats& stats() const noexcept { return stats_; }

private:
    enum class Move { swap, insertion, reversal };
    [[nodiscard]] std::vector<std::uint32_t> evaluate(Move move, std::size_t a, std::size_t b);
    void apply(Move move, std::size_t a, std::size_t b);
    void rebuild_positions();
    void rebuild_histogram();
    const Graph& graph_;
    std::vector<Graph::Vertex> order_;
    std::vector<std::size_t> positions_;
    std::vector<std::uint32_t> cuts_;
    std::vector<std::uint32_t> histogram_;
    std::uint32_t maximum_ = 0;
    IncrementalLayoutStats stats_;
};

[[nodiscard]] bool cut_profile_better(const std::vector<std::uint32_t>& candidate,
                                      const std::vector<std::uint32_t>& incumbent);

} // namespace cutwidth
