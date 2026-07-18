#include "graph.hpp"
#include "incremental_layout.hpp"
#include "node_memo.hpp"
#include "decision.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
using cutwidth::Graph;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

Graph graph_from_bits(std::uint32_t n, std::uint64_t bits) {
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    std::uint32_t edge = 0;
    for (Graph::Vertex u = 0; u < n; ++u)
        for (Graph::Vertex v = u + 1; v < n; ++v, ++edge)
            if ((bits >> edge) & 1U) edges.emplace_back(u, v);
    return Graph(n, edges);
}

std::uint32_t explicit_h(const Graph& graph, Graph::Mask prefix, std::uint8_t depth) {
    const auto delta = graph.cut(prefix);
    const auto all = graph.size() == 0 ? 0 : (Graph::Mask{1} << graph.size()) - 1;
    if (depth == 0 || prefix == all) return delta;
    std::uint32_t minimum = UINT32_MAX;
    auto remaining = all & ~prefix;
    while (remaining) {
        const auto v = static_cast<Graph::Vertex>(std::countr_zero(remaining));
        remaining &= remaining - 1;
        minimum = std::min(minimum, explicit_h(graph, prefix | (Graph::Mask{1} << v), depth - 1));
    }
    return std::max(delta, minimum);
}

std::uint32_t exact_completion(const Graph& graph, Graph::Mask prefix) {
    const auto all = graph.size() == 0 ? 0 : (Graph::Mask{1} << graph.size()) - 1;
    return explicit_h(graph, prefix, static_cast<std::uint8_t>(std::popcount(all & ~prefix)));
}

void oracle_exhaustive() {
    for (std::uint32_t n = 0; n <= 6; ++n) {
        const auto pairs = n * (n - 1) / 2;
        const auto count = std::uint64_t{1} << pairs;
        for (std::uint64_t bits = 0; bits < count; ++bits) {
            const auto graph = graph_from_bits(n, bits);
            auto table = std::make_shared<cutwidth::NodeMemoTable>(1U << 16, 4);
            cutwidth::FiniteHorizonOracle cached(graph, table), uncached(graph);
            const auto prefixes = std::uint64_t{1} << n;
            for (Graph::Mask prefix = 0; prefix < prefixes; ++prefix) {
                std::uint32_t prior = 0;
                const auto completion = exact_completion(graph, prefix);
                for (std::uint8_t depth = 0; depth <= 4; ++depth) {
                    const auto expected = explicit_h(graph, prefix, depth);
                    const auto a = cached.evaluate(prefix, depth);
                    const auto b = uncached.evaluate(prefix, depth);
                    require(a.complete && b.complete, "completed oracle marked incomplete");
                    require(a.bound == expected && b.bound == expected, "finite-horizon recurrence mismatch");
                    require(a.bound >= prior, "finite-horizon bound is not monotone");
                    require(a.bound <= completion, "finite-horizon bound exceeds exact completion");
                    prior = a.bound;
                }
            }
        }
    }
}

std::vector<std::uint32_t> full_cuts(const Graph& graph,
                                     const std::vector<Graph::Vertex>& order) {
    std::vector<std::uint32_t> cuts(order.size());
    Graph::Mask prefix = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        prefix |= Graph::Mask{1} << order[i]; cuts[i] = graph.cut(prefix);
    }
    return cuts;
}

void layout_differential() {
    std::mt19937_64 rng(0x173A9ULL);
    for (std::uint32_t n = 2; n <= 8; ++n) {
        for (int sample = 0; sample < 20; ++sample) {
            std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
            for (Graph::Vertex u = 0; u < n; ++u)
                for (Graph::Vertex v = u + 1; v < n; ++v)
                    if ((rng() & 3U) == 0) edges.emplace_back(u, v);
            Graph graph(n, edges);
            std::vector<Graph::Vertex> order(n);
            std::iota(order.begin(), order.end(), 0); std::shuffle(order.begin(), order.end(), rng);
            cutwidth::IncrementalLayoutEvaluator eval(graph, order);
            for (std::size_t a = 0; a < n; ++a) for (std::size_t b = 0; b < n; ++b) {
                auto swapped = order; std::swap(swapped[a], swapped[b]);
                require(eval.cuts_after_swap(a, b) == full_cuts(graph, swapped), "swap delta mismatch");
                auto inserted = order; const auto v = inserted[a];
                inserted.erase(inserted.begin() + static_cast<std::ptrdiff_t>(a));
                inserted.insert(inserted.begin() + static_cast<std::ptrdiff_t>(b), v);
                require(eval.cuts_after_insertion(a, b) == full_cuts(graph, inserted), "insertion delta mismatch");
                auto reversed = order; const auto lo = std::min(a,b), hi = std::max(a,b);
                std::reverse(reversed.begin() + static_cast<std::ptrdiff_t>(lo),
                             reversed.begin() + static_cast<std::ptrdiff_t>(hi + 1));
                require(eval.cuts_after_reversal(a, b) == full_cuts(graph, reversed), "reversal delta mismatch");
            }
        }
    }
}

void safety_and_decision_equivalence() {
    const auto graph = graph_from_bits(6, 0x5A73);
    auto tiny = std::make_shared<cutwidth::NodeMemoTable>(sizeof(cutwidth::NodeMemoValue) * 2, 1);
    cutwidth::FiniteHorizonOracle oracle(graph, tiny);
    for (Graph::Mask prefix = 0; prefix < 64; ++prefix)
        require(oracle.evaluate(prefix, 2).bound == explicit_h(graph, prefix, 2), "collision changed certificate");
    require(tiny->stats().collisions != 0 && tiny->stats().saturation != 0,
            "tiny table did not exercise collision/saturation");
    cutwidth::FiniteHorizonOracle interrupted(graph);
    const auto partial = interrupted.evaluate(0, 4, std::chrono::steady_clock::now());
    require(!partial.complete && partial.bound == graph.cut(0), "interrupted oracle overclaimed completion");

    for (std::uint8_t depth = 0; depth <= 2; ++depth) {
        for (std::uint32_t k = 0; k <= graph.edge_count(); ++k) {
            const auto old = cutwidth::decide_cutwidth(graph, k);
            cutwidth::DecisionOptions options;
            options.node_state = cutwidth::NodeStateMode::incremental;
            options.node_order = depth == 0 ? cutwidth::NodeOrder::cut : cutwidth::NodeOrder::memo;
            options.node_memo_depth = depth;
            options.node_memo_memory_bytes = 1U << 16;
            const auto next = cutwidth::decide_cutwidth(graph, k, options);
            require(old.status == next.status, "incremental/memo decision changed result");
            if (depth == 2 && (k == 0 || k == graph.edge_count() / 2)) {
                options.threads = 8;
                options.cache_mode = cutwidth::CacheMode::fixed_threshold;
                const auto parallel_fixed = cutwidth::decide_cutwidth(graph, k, options);
                require(old.status == parallel_fixed.status,
                        "parallel fixed-cache memo decision changed result");
                options.cache_mode = cutwidth::CacheMode::cross_threshold;
                const auto parallel_cross = cutwidth::decide_cutwidth(graph, k, options);
                require(old.status == parallel_cross.status,
                        "parallel cross-cache memo decision changed result");
            }
        }
    }
}
}

int main() {
    try {
        oracle_exhaustive();
        layout_differential();
        safety_and_decision_equivalence();
        std::cout << "node memo and incremental layout tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n'; return 1;
    }
}
