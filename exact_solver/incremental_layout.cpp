#include "incremental_layout.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace cutwidth {
namespace {
void mutate(std::vector<Graph::Vertex>& order, int move, std::size_t a, std::size_t b) {
    if (a >= order.size() || b >= order.size()) throw std::out_of_range("layout move index");
    if (move == 0) std::swap(order[a], order[b]);
    else if (move == 1) {
        const auto v = order[a];
        order.erase(order.begin() + static_cast<std::ptrdiff_t>(a));
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(b), v);
    } else {
        if (a > b) std::swap(a, b);
        std::reverse(order.begin() + static_cast<std::ptrdiff_t>(a),
                     order.begin() + static_cast<std::ptrdiff_t>(b + 1));
    }
}
}

IncrementalLayoutEvaluator::IncrementalLayoutEvaluator(
    const Graph& graph, std::vector<Graph::Vertex> ordering)
    : graph_(graph), order_(std::move(ordering)), positions_(graph.size()),
      cuts_(graph.size(), 0) {
    if (!graph_.validate_ordering(order_)) throw std::invalid_argument("invalid layout ordering");
    rebuild_positions();
    Graph::Mask prefix = 0;
    if (graph_.supports_mask()) {
        for (std::size_t i = 0; i < order_.size(); ++i) {
            prefix |= Graph::Mask{1} << order_[i];
            cuts_[i] = graph_.cut(prefix);
        }
    } else {
        std::vector<Graph::Mask> words(graph_.word_count(), 0);
        for (std::size_t i = 0; i < order_.size(); ++i) {
            words[order_[i] / 64U] |= Graph::Mask{1} << (order_[i] % 64U);
            cuts_[i] = graph_.cut(words);
        }
    }
    rebuild_histogram();
}

void IncrementalLayoutEvaluator::rebuild_positions() {
    for (std::size_t i = 0; i < order_.size(); ++i) positions_[order_[i]] = i;
}

void IncrementalLayoutEvaluator::rebuild_histogram() {
    maximum_ = cuts_.empty() ? 0 : *std::max_element(cuts_.begin(), cuts_.end());
    histogram_.assign(static_cast<std::size_t>(maximum_) + 1, 0);
    for (auto cut : cuts_) ++histogram_[cut];
}

std::vector<std::uint32_t> IncrementalLayoutEvaluator::descending_cut_profile() const {
    auto result = cuts_;
    std::sort(result.begin(), result.end(), std::greater<>());
    return result;
}

std::vector<std::uint32_t> IncrementalLayoutEvaluator::evaluate(
    Move move, std::size_t a, std::size_t b) {
    ++stats_.interval_evaluations;
    auto candidate = order_;
    mutate(candidate, static_cast<int>(move), a, b);
    auto result = cuts_;
    auto left = std::min(a, b), right = std::max(a, b);
    // Prefixes outside [left,right] contain the same vertex set. Recompute only
    // that interval, using the maintained position map for edge membership.
    std::vector<std::size_t> candidate_positions(graph_.size());
    for (std::size_t i = 0; i < candidate.size(); ++i)
        candidate_positions[candidate[i]] = i;
    std::uint32_t crossing = left == 0 ? 0 : cuts_[left - 1];
    for (std::size_t cut = left; cut <= right; ++cut) {
        const auto vertex = candidate[cut];
        std::uint32_t before = 0;
        const auto words = graph_.adjacency_words(vertex);
        for (std::size_t word_index = 0; word_index < words.size(); ++word_index) {
            auto word = words[word_index];
            while (word) {
                const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
                word &= word - 1;
                const auto neighbor = static_cast<Graph::Vertex>(word_index * 64U + bit);
                if (neighbor < graph_.size() && candidate_positions[neighbor] < cut) ++before;
            }
        }
        crossing = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(crossing) + graph_.degree(vertex) - 2 * before);
        result[cut] = crossing;
    }
    return result;
}

void IncrementalLayoutEvaluator::apply(Move move, std::size_t a, std::size_t b) {
    auto next = evaluate(move, a, b);
    mutate(order_, static_cast<int>(move), a, b);
    cuts_ = std::move(next);
    rebuild_positions();
    rebuild_histogram();
}

std::vector<std::uint32_t> IncrementalLayoutEvaluator::cuts_after_swap(std::size_t a, std::size_t b) { return evaluate(Move::swap, a, b); }
std::vector<std::uint32_t> IncrementalLayoutEvaluator::cuts_after_insertion(std::size_t a, std::size_t b) { return evaluate(Move::insertion, a, b); }
std::vector<std::uint32_t> IncrementalLayoutEvaluator::cuts_after_reversal(std::size_t a, std::size_t b) { return evaluate(Move::reversal, a, b); }
void IncrementalLayoutEvaluator::apply_swap(std::size_t a, std::size_t b) { apply(Move::swap, a, b); }
void IncrementalLayoutEvaluator::apply_insertion(std::size_t a, std::size_t b) { apply(Move::insertion, a, b); }
void IncrementalLayoutEvaluator::apply_reversal(std::size_t a, std::size_t b) { apply(Move::reversal, a, b); }

bool cut_profile_better(const std::vector<std::uint32_t>& candidate,
                        const std::vector<std::uint32_t>& incumbent) {
    auto a = candidate, b = incumbent;
    std::sort(a.begin(), a.end(), std::greater<>());
    std::sort(b.begin(), b.end(), std::greater<>());
    return a < b;
}

} // namespace cutwidth
