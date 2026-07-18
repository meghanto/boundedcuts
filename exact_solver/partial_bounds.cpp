#include "partial_bounds.hpp"
#include "vertex_set.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cutwidth {

std::uint32_t average_degree_lower_bound(std::size_t vertex_count,
                                         std::size_t edge_count) {
    if (vertex_count <= 1 || edge_count == 0) return 0;
    if (vertex_count > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("vertex count exceeds the solver's vertex type");
    const std::uint64_t n = vertex_count;
    const std::uint64_t m = edge_count;
    const std::uint64_t maximum_edges = n * (n - 1) / 2;
    if (m > maximum_edges)
        throw std::invalid_argument("edge count exceeds a simple graph's capacity");

    // Evaluate ceil(m(m+d)/(2d^2)), d=n-1, without non-standard 128-bit
    // arithmetic. Write m=q*d+r. The integral part is q(q+1)/2; the
    // remaining two fractions are r(2q+1)/(2d) + r^2/(2d^2).
    const std::uint64_t d = n - 1;
    const std::uint64_t q = m / d;
    const std::uint64_t r = m % d;
    std::uint64_t result = q * (q + 1) / 2;
    const std::uint64_t twice_d = 2 * d;
    const std::uint64_t first_numerator = r * (2 * q + 1);
    result += first_numerator / twice_d;
    if (r != 0) {
        const std::uint64_t first_remainder = first_numerator % twice_d;
        const std::uint64_t r_squared = r * r;
        const std::uint64_t reduced_sum = first_remainder + r_squared / d;
        // The combined proper fractions lie in (0, 1.5), so their ceiling is
        // one or two. Compare against one after dividing the common numerator
        // by d, which keeps every intermediate within uint64_t.
        const bool exceeds_one = reduced_sum > twice_d ||
            (reduced_sum == twice_d && r_squared % d != 0);
        result += exceeds_one ? 2 : 1;
    }
    if (result > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("average-degree lower bound exceeds uint32_t");
    return static_cast<std::uint32_t>(result);
}

namespace {

std::vector<std::uint32_t> grooming_weights(std::size_t n) {
    std::vector<std::uint32_t> weights;
    if (n < 2) return weights;
    const auto count = (n / 2) * (n - n / 2);
    weights.reserve(count);
    for (std::size_t sigma = 1; sigma < n; ++sigma) {
        const auto last_t = std::min(sigma, n - sigma);
        for (std::size_t t = 1; t <= last_t; ++t)
            weights.push_back(static_cast<std::uint32_t>((n - t) / sigma));
    }
    std::sort(weights.begin(), weights.end(), std::greater<>());
    return weights;
}

} // namespace

std::uint64_t grooming_request_capacity(std::size_t vertex_count,
                                        std::uint32_t cut_capacity) {
    if (vertex_count < 2 || cut_capacity == 0) return 0;
    const auto saturation = (vertex_count / 2) * (vertex_count - vertex_count / 2);
    if (cut_capacity >= saturation)
        return static_cast<std::uint64_t>(vertex_count) * (vertex_count - 1) / 2;
    const auto weights = grooming_weights(vertex_count);
    std::uint64_t requests = 0;
    for (std::size_t i = 0; i < cut_capacity; ++i) requests += weights[i];
    return requests;
}

std::uint32_t grooming_density_lower_bound(std::size_t vertex_count,
                                           std::size_t edge_count) {
    if (vertex_count < 2 || edge_count == 0) return 0;
    const auto maximum_edges = static_cast<std::uint64_t>(vertex_count) *
        (vertex_count - 1) / 2;
    if (edge_count > maximum_edges)
        throw std::invalid_argument("edge count exceeds a simple graph's capacity");
    const auto weights = grooming_weights(vertex_count);
    std::uint64_t requests = 0;
    for (std::size_t capacity = 0; capacity < weights.size(); ++capacity) {
        requests += weights[capacity];
        if (requests >= edge_count) {
            if (capacity + 1 > std::numeric_limits<std::uint32_t>::max())
                throw std::overflow_error("grooming lower bound exceeds uint32_t");
            return static_cast<std::uint32_t>(capacity + 1);
        }
    }
    if (weights.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("grooming lower bound exceeds uint32_t");
    return static_cast<std::uint32_t>(weights.size());
}

namespace {

std::uint32_t ceil_div(std::uint64_t value, std::uint32_t divisor) {
    if (divisor == 0) return 0;
    return static_cast<std::uint32_t>((value + divisor - 1) / divisor);
}

struct ResidualData {
    std::uint32_t count = 0;
    std::uint32_t maximum_degree = 0;
    std::uint64_t boundary_area = 0;
    std::uint64_t minimum_edge_distance = 0;
    std::uint64_t minimum_degree_distance = 0;
    VertexSet remaining;
};

bool contains(std::span<const Graph::Mask> set, Graph::Vertex vertex) {
    return (set[static_cast<std::size_t>(vertex) / 64U] &
            (Graph::Mask{1} << (vertex % 64U))) != 0;
}

ResidualData residual_data(const Graph& graph, std::span<const Graph::Mask> prefix) {
    ResidualData data;
    data.remaining = VertexSet(graph.size(), true);
    for (Graph::Vertex v = 0; v < graph.size(); ++v)
        if (contains(prefix, v)) data.remaining.erase(v);
    data.count = static_cast<std::uint32_t>(data.remaining.count());

    std::vector<std::uint32_t> boundary_degrees;
    boundary_degrees.reserve(data.count);
    std::uint64_t residual_degree_sum = 0;
    if (graph.supports_mask()) {
        const auto prefix_word = prefix.empty() ? Graph::Mask{0} : prefix.front();
        const auto all = graph.size() == 0 ? Graph::Mask{0} :
            (Graph::Mask{1} << graph.size()) - 1;
        const auto remaining_word = all & ~prefix_word;
        auto vertices = remaining_word;
        while (vertices != 0) {
            const auto v = static_cast<Graph::Vertex>(std::countr_zero(vertices));
            vertices &= vertices - 1;
            const auto adjacency = graph.adjacency(v);
            const auto a = static_cast<std::uint32_t>(std::popcount(adjacency & prefix_word));
            const auto d = static_cast<std::uint32_t>(std::popcount(adjacency & remaining_word));
            boundary_degrees.push_back(a);
            data.maximum_degree = std::max(data.maximum_degree, d);
            residual_degree_sum += d;
            const std::uint64_t x = static_cast<std::uint64_t>(d) + 1;
            data.minimum_degree_distance += (x * x) / 4;
        }
    } else {
    for (Graph::Vertex v = 0; v < graph.size(); ++v) {
        if (!data.remaining.contains(v)) continue;
        std::uint32_t a = 0;
        std::uint32_t d = 0;
        for (Graph::Vertex other = 0; other < graph.size(); ++other) {
            if (!graph.adjacent(v, other)) continue;
            if (contains(prefix, other)) ++a;
            else if (data.remaining.contains(other)) ++d;
        }
        boundary_degrees.push_back(a);
        data.maximum_degree = std::max(data.maximum_degree, d);
        residual_degree_sum += d;
        // Among d distinct other positions, the minimum sum of distances from
        // one position is floor((d+1)^2/4). Summing over vertices counts every
        // edge distance twice, hence the division by two below.
        const std::uint64_t x = static_cast<std::uint64_t>(d) + 1;
        data.minimum_degree_distance += (x * x) / 4;
    }
    }
    data.minimum_degree_distance = (data.minimum_degree_distance + 1) / 2;

    // A boundary edge survives until its unplaced endpoint is selected. By the
    // rearrangement inequality, assigning larger boundary degrees to earlier
    // positions minimizes their total contribution to all future cut areas.
    std::sort(boundary_degrees.begin(), boundary_degrees.end(), std::greater<>());
    for (std::uint32_t i = 0; i < data.count; ++i)
        data.boundary_area += static_cast<std::uint64_t>(i + 1) * boundary_degrees[i];

    // Relax the residual graph to any simple graph with the same edge count.
    // There are r-d vertex pairs at distance d; greedily occupying the shortest
    // distances gives a lower bound on total residual edge length.
    std::uint64_t edges_left = residual_degree_sum / 2;
    for (std::uint32_t distance = 1; distance < data.count && edges_left; ++distance) {
        const std::uint64_t slots = data.count - distance;
        const std::uint64_t used = std::min(edges_left, slots);
        data.minimum_edge_distance += used * distance;
        edges_left -= used;
    }
    return data;
}

std::uint32_t residual_degeneracy(const Graph& graph, VertexSet remaining) {
    std::uint32_t result = 0;
    while (!remaining.empty()) {
        Graph::Vertex best = 0;
        std::uint32_t best_degree = UINT32_MAX;
        for (Graph::Vertex v = 0; v < graph.size(); ++v) {
            if (!remaining.contains(v)) continue;
            std::uint32_t degree = 0;
            for (Graph::Vertex other = 0; other < graph.size(); ++other)
                if (remaining.contains(other) && graph.adjacent(v, other)) ++degree;
            if (degree < best_degree) {
                best = v;
                best_degree = degree;
            }
        }
        result = std::max(result, best_degree);
        remaining.erase(best);
    }
    return result;
}

bool is_triangle_free(const Graph& graph) {
    for (Graph::Vertex u = 0; u < graph.size(); ++u) {
        const auto u_adj = graph.adjacency_words(u);
        for (Graph::Vertex v = u + 1; v < graph.size(); ++v) {
            if (!graph.adjacent(u, v)) continue;
            const auto v_adj = graph.adjacency_words(v);
            for (std::size_t word = 0; word < u_adj.size(); ++word)
                if ((u_adj[word] & v_adj[word]) != 0) return false;
        }
    }
    return true;
}

std::uint32_t degeneracy_cutwidth_bound(std::uint32_t degeneracy,
                                        bool triangle_free) {
    const auto d = static_cast<std::uint64_t>(degeneracy);
    const auto bound = triangle_free ? (d * d + 1) / 2
                                     : ((d + 1) * (d + 1)) / 4;
    if (bound > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("degeneracy cutwidth bound exceeds uint32_t");
    return static_cast<std::uint32_t>(bound);
}

} // namespace

std::uint32_t PartialBoundEvaluator::residual_degree_session_ceiling(
    const Graph& graph) {
    std::uint32_t maximum = 0;
    for (Graph::Vertex v = 0; v < graph.size(); ++v)
        maximum = std::max(maximum, graph.degree(v));
    return (maximum + 1) / 2;
}

std::uint32_t PartialBoundEvaluator::degeneracy_session_ceiling(
    const Graph& graph) {
    const auto d = static_cast<std::uint64_t>(
        residual_degeneracy(graph, VertexSet(graph.size(), true)));
    return degeneracy_cutwidth_bound(static_cast<std::uint32_t>(d),
                                     is_triangle_free(graph));
}

PartialBoundEvaluator::PartialBoundEvaluator(
    const Graph& graph, PartialBoundOptions options,
    std::optional<std::uint32_t> threshold)
    : graph_(graph), options_(options), triangle_free_(
          options.degeneracy && is_triangle_free(graph)) {
    if (!threshold) return;
    if (options_.residual_degree &&
        residual_degree_session_ceiling(graph_) <= *threshold) {
        options_.residual_degree = false;
        residual_degree_skipped_ = true;
    }
    if (options_.degeneracy && degeneracy_session_ceiling(graph_) <= *threshold) {
        options_.degeneracy = false;
        degeneracy_skipped_ = true;
    }
}

void PartialBoundEvaluator::note_session_ceiling_skips(
    PartialBoundStats& stats) const noexcept {
    if (residual_degree_skipped_) ++stats.residual_degree_session_ceiling_skips;
    if (degeneracy_skipped_) ++stats.degeneracy_session_ceiling_skips;
}

PartialBoundValues PartialBoundEvaluator::evaluate(Graph::Mask prefix,
                                                   std::uint32_t depth) const {
    const Graph::Mask word = prefix;
    if (graph_.word_count() == 0)
        return evaluate(std::span<const Graph::Mask>{}, depth);
    return evaluate(std::span<const Graph::Mask>(&word, 1), depth);
}

PartialBoundValues PartialBoundEvaluator::evaluate(
    std::span<const Graph::Mask> prefix, std::uint32_t depth) const {
    PartialBoundValues values;
    values.current_cut = graph_.cut(prefix);
    values.combined = values.current_cut;
    const auto data = residual_data(graph_, prefix);
    if (data.count == 0) return values;

    if (options_.residual_degree) {
        values.residual_degree = (data.maximum_degree + 1) / 2;
        values.combined = std::max(values.combined, values.residual_degree);
    }
    if (options_.edge_distance_area) {
        values.edge_distance_area = ceil_div(
            data.boundary_area + data.minimum_edge_distance, data.count);
        values.combined = std::max(values.combined, values.edge_distance_area);
    }
    if (options_.degree_distance_area) {
        values.degree_distance_area = ceil_div(
            data.boundary_area + data.minimum_degree_distance, data.count);
        values.combined = std::max(values.combined, values.degree_distance_area);
    }
    (void)depth;
    if (options_.degeneracy) {
        const std::uint64_t d = residual_degeneracy(graph_, data.remaining);
        // Kloeckner: a d-degenerate lower witness implies
        // cutwidth >= floor((d+1)^2/4) for the residual induced graph.
        values.degeneracy = degeneracy_cutwidth_bound(
            static_cast<std::uint32_t>(d), triangle_free_);
        values.combined = std::max(values.combined, values.degeneracy);
    }
    return values;
}

bool PartialBoundEvaluator::exceeds(Graph::Mask prefix, std::uint32_t depth,
                                    std::uint32_t threshold,
                                    PartialBoundStats& stats) const {
    const Graph::Mask word = prefix;
    if (graph_.word_count() == 0)
        return exceeds(std::span<const Graph::Mask>{}, depth, threshold, stats);
    return exceeds(std::span<const Graph::Mask>(&word, 1), depth, threshold, stats);
}

bool PartialBoundEvaluator::exceeds(std::span<const Graph::Mask> prefix,
                                    std::uint32_t depth,
                                    std::uint32_t threshold,
                                    PartialBoundStats& stats) const {
    if (graph_.cut(prefix) > threshold) return true;
    return completion_exceeds(prefix, depth, threshold, stats, std::nullopt,
                              graph_.cut(prefix));
}

bool PartialBoundEvaluator::completion_exceeds(
    std::span<const Graph::Mask> prefix, std::uint32_t depth,
    std::uint32_t threshold, PartialBoundStats& stats,
    std::optional<std::uint32_t> known_maximum_residual_degree,
    std::optional<std::uint32_t> known_current_cut) const {
    if (!options_.residual_degree && !options_.edge_distance_area &&
        !options_.degree_distance_area && !options_.degeneracy)
        return false;
    ++stats.evaluations;
    const bool expensive_configured = options_.edge_distance_area ||
        options_.degree_distance_area || options_.degeneracy;
    const bool expensive_enabled = !expensive_configured || !known_current_cut ||
        *known_current_cut > threshold ||
        threshold - *known_current_cut <= options_.expensive_max_slack;
    if (expensive_configured && !expensive_enabled)
        ++stats.expensive_slack_gate_skips;
    if (options_.residual_degree && (!expensive_configured || !expensive_enabled)) {
        ++stats.residual_degree_evaluations;
        std::uint32_t maximum = known_maximum_residual_degree.value_or(0);
        if (!known_maximum_residual_degree && graph_.supports_mask()) {
            const auto p = prefix.empty() ? Graph::Mask{0} : prefix.front();
            const auto all = graph_.size() == 0 ? Graph::Mask{0} :
                (Graph::Mask{1} << graph_.size()) - 1;
            const auto remaining = all & ~p;
            auto scan = remaining;
            while (scan != 0) {
                const auto v = static_cast<Graph::Vertex>(std::countr_zero(scan));
                scan &= scan - 1;
                maximum = std::max(maximum, static_cast<std::uint32_t>(
                    std::popcount(graph_.adjacency(v) & remaining)));
            }
        } else if (!known_maximum_residual_degree) {
            for (Graph::Vertex v = 0; v < graph_.size(); ++v) {
                if (contains(prefix, v)) continue;
                std::uint32_t degree = 0;
                const auto adjacency = graph_.adjacency_words(v);
                for (std::size_t word = 0; word < graph_.word_count(); ++word)
                    degree += static_cast<std::uint32_t>(std::popcount(adjacency[word] & ~prefix[word]));
                maximum = std::max(maximum, degree);
            }
        }
        if ((maximum + 1) / 2 > threshold) {
            ++stats.residual_degree_prunes;
            return true;
        }
        return false;
    }
    if (!expensive_enabled) return false;
    const auto data = residual_data(graph_, prefix);
    if (data.count == 0) return false;
    if (options_.residual_degree) {
        ++stats.residual_degree_evaluations;
        if ((data.maximum_degree + 1) / 2 > threshold) {
            ++stats.residual_degree_prunes;
            return true;
        }
    }
    if (options_.edge_distance_area) {
        ++stats.edge_distance_area_evaluations;
        if (ceil_div(data.boundary_area + data.minimum_edge_distance, data.count) > threshold) {
            ++stats.edge_distance_area_prunes;
            return true;
        }
    }
    if (options_.degree_distance_area) {
        ++stats.degree_distance_area_evaluations;
        if (ceil_div(data.boundary_area + data.minimum_degree_distance, data.count) > threshold) {
            ++stats.degree_distance_area_prunes;
            return true;
        }
    }
    (void)depth;
    if (options_.degeneracy) {
        ++stats.degeneracy_evaluations;
        const std::uint64_t d = residual_degeneracy(graph_, data.remaining);
        if (degeneracy_cutwidth_bound(
                static_cast<std::uint32_t>(d), triangle_free_) > threshold) {
            ++stats.degeneracy_prunes;
            return true;
        }
    }
    return false;
}

} // namespace cutwidth
