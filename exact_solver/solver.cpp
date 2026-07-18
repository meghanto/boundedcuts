#include "solver.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <numeric>
#include <optional>
#include <unordered_map>

namespace cutwidth {
namespace {

using Vertex = Graph::Vertex;
using Mask = Graph::Mask;

bool deadline_reached(const std::chrono::steady_clock::time_point start,
                      const std::chrono::milliseconds limit) {
    return limit.count() != 0 && std::chrono::steady_clock::now() - start >= limit;
}

std::optional<std::vector<Vertex>> greedy_order(
    const Graph& graph, Vertex first,
    const std::chrono::steady_clock::time_point start,
    const std::chrono::milliseconds limit) {
    const std::size_t n = graph.size();
    const Mask all = n == 0 ? 0 : (Mask{1} << n) - 1;
    std::vector<Vertex> order;
    order.reserve(n);
    Mask prefix = 0;
    std::uint32_t cut = 0;
    if (n != 0) {
        order.push_back(first);
        prefix |= Mask{1} << first;
        cut = graph.degree(first);
    }
    while (order.size() < n) {
        if (deadline_reached(start, limit)) return std::nullopt;
        const Mask remaining = all & ~prefix;
        Vertex best = 0;
        std::uint32_t best_next = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t best_future_degree = std::numeric_limits<std::uint32_t>::max();
        Mask choices = remaining;
        while (choices) {
            if (deadline_reached(start, limit)) return std::nullopt;
            const Vertex v = static_cast<Vertex>(std::countr_zero(choices));
            choices &= choices - 1;
            const auto to_prefix = static_cast<std::uint32_t>(std::popcount(graph.adjacency(v) & prefix));
            const auto to_remaining = static_cast<std::uint32_t>(std::popcount(graph.adjacency(v) & (remaining & ~(Mask{1} << v))));
            const auto next = static_cast<std::uint32_t>(static_cast<std::int64_t>(cut) + to_remaining - to_prefix);
            if (next < best_next || (next == best_next && to_remaining < best_future_degree) ||
                (next == best_next && to_remaining == best_future_degree && v < best)) {
                best = v;
                best_next = next;
                best_future_degree = to_remaining;
            }
        }
        order.push_back(best);
        prefix |= Mask{1} << best;
        cut = best_next;
    }
    return order;
}

bool adjacent_swap_descent(const Graph& graph, std::vector<Vertex>& order,
                           const std::chrono::steady_clock::time_point start,
                           const std::chrono::milliseconds limit) {
    if (order.size() < 2) return true;
    std::uint32_t width = graph.ordering_cutwidth(order);
    bool improved = true;
    while (improved) {
        if (deadline_reached(start, limit)) return false;
        improved = false;
        for (std::size_t i = 0; i + 1 < order.size(); ++i) {
            if (deadline_reached(start, limit)) return false;
            std::swap(order[i], order[i + 1]);
            const auto candidate = graph.ordering_cutwidth(order);
            if (candidate < width) {
                width = candidate;
                improved = true;
            } else {
                std::swap(order[i], order[i + 1]);
            }
        }
    }
    return true;
}

std::pair<std::uint32_t, std::vector<Vertex>> initial_incumbent(
    const Graph& graph, const std::chrono::steady_clock::time_point start,
    const std::chrono::milliseconds limit, bool& timed_out) {
    if (graph.size() == 0) return {0, {}};
    // Keep a complete incumbent from the outset so an arbitrarily short time
    // limit can still return a valid feasible ordering.
    std::vector<Vertex> best(graph.size());
    std::iota(best.begin(), best.end(), Vertex{0});
    std::uint32_t best_width = graph.ordering_cutwidth(best);
    for (Vertex first = 0; first < graph.size(); ++first) {
        if (deadline_reached(start, limit)) { timed_out = true; break; }
        auto candidate = greedy_order(graph, first, start, limit);
        if (!candidate) { timed_out = true; break; }
        auto order = std::move(*candidate);
        if (!adjacent_swap_descent(graph, order, start, limit)) {
            timed_out = true;
            break;
        }
        const auto width = graph.ordering_cutwidth(order);
        if (width < best_width || (width == best_width && order < best)) {
            best_width = width;
            best = std::move(order);
        }
    }
    return {best_width, std::move(best)};
}

class Search {
public:
    Search(const Graph& graph, const SolverOptions& options)
        : graph(graph), options(options), n(graph.size()),
          all(n == 0 ? 0 : (Mask{1} << n) - 1), start(std::chrono::steady_clock::now()) {
        auto incumbent = initial_incumbent(graph, start, options.time_limit, timed_out);
        best_width = incumbent.first;
        best_order = std::move(incumbent.second);
        path.reserve(n);
    }

    SolverResult run() {
        dfs(0, 0, 0);
        SolverResult result;
        result.optimal = !timed_out;
        // On interruption, the universal maximum-degree bound remains a valid
        // certificate. A completed exhaustive search closes the gap.
        result.lower_bound = result.optimal ? best_width : root_lower_bound();
        result.upper_bound = best_width;
        result.ordering = std::move(best_order);
        result.nodes_expanded = nodes;
        result.pruned_by_bound = bound_prunes;
        result.pruned_by_dominance = dominance_prunes;
        result.transposition_table_size = dominance.size();
        return result;
    }

private:
    std::uint32_t induced_degree_bound(Mask remaining) const {
        std::uint32_t max_degree = 0;
        Mask scan = remaining;
        while (scan) {
            const Vertex v = static_cast<Vertex>(std::countr_zero(scan));
            scan &= scan - 1;
            max_degree = std::max(max_degree,
                static_cast<std::uint32_t>(std::popcount(graph.adjacency(v) & remaining)));
        }
        return (max_degree + 1) / 2;
    }

    std::uint32_t root_lower_bound() const { return induced_degree_bound(all); }

    bool expired() {
        return deadline_reached(start, options.time_limit);
    }

    void dfs(Mask prefix, std::uint32_t current_cut, std::uint32_t maximum_seen) {
        if (timed_out) return;
        ++nodes;
        if (expired()) { timed_out = true; return; }

        const Mask remaining = all & ~prefix;
        const std::uint32_t lower = std::max(maximum_seen, induced_degree_bound(remaining));
        if (lower >= best_width) { ++bound_prunes; return; }
        if (remaining == 0) {
            best_width = maximum_seen;
            best_order = path;
            return;
        }

        if (options.use_transposition_table) {
            const auto found = dominance.find(prefix);
            if (found != dominance.end() && found->second <= maximum_seen) {
                ++dominance_prunes;
                return;
            }
            if (found != dominance.end()) {
                found->second = maximum_seen;
            } else if (options.transposition_table_limit == 0 || dominance.size() < options.transposition_table_limit) {
                dominance.emplace(prefix, maximum_seen);
            }
        }

        struct Candidate { Vertex vertex; std::uint32_t cut; std::uint32_t maximum; };
        std::vector<Candidate> candidates;
        candidates.reserve(std::popcount(remaining));
        Mask choices = remaining;
        while (choices) {
            const Vertex v = static_cast<Vertex>(std::countr_zero(choices));
            choices &= choices - 1;
            const auto to_prefix = static_cast<std::uint32_t>(std::popcount(graph.adjacency(v) & prefix));
            const auto to_remaining = static_cast<std::uint32_t>(std::popcount(graph.adjacency(v) & (remaining & ~(Mask{1} << v))));
            const auto next_cut = static_cast<std::uint32_t>(static_cast<std::int64_t>(current_cut) + to_remaining - to_prefix);
            candidates.push_back({v, next_cut, std::max(maximum_seen, next_cut)});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.maximum != b.maximum) return a.maximum < b.maximum;
            if (a.cut != b.cut) return a.cut < b.cut;
            return a.vertex < b.vertex;
        });

        for (const auto& candidate : candidates) {
            if (candidate.maximum >= best_width) { ++bound_prunes; continue; }
            path.push_back(candidate.vertex);
            dfs(prefix | (Mask{1} << candidate.vertex), candidate.cut, candidate.maximum);
            path.pop_back();
            if (timed_out) return;
        }
    }

    const Graph& graph;
    const SolverOptions& options;
    const std::size_t n;
    const Mask all;
    const std::chrono::steady_clock::time_point start;
    std::uint32_t best_width;
    std::vector<Vertex> best_order;
    std::vector<Vertex> path;
    std::unordered_map<Mask, std::uint32_t> dominance;
    std::uint64_t nodes = 0, bound_prunes = 0, dominance_prunes = 0;
    bool timed_out = false;
};

} // namespace

ExactSolver::ExactSolver(const Graph& graph, SolverOptions options)
    : graph_(graph), options_(options) {}

SolverResult ExactSolver::solve() {
    return Search(graph_, options_).run();
}

} // namespace cutwidth
