#include "sdp_operator.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cutwidth::sdp {

PairIndex::PairIndex(std::size_t vertex_count) : n_(vertex_count) {
    if (n_ > 1 && n_ > std::numeric_limits<std::size_t>::max() / (n_ - 1))
        throw std::length_error("pair count overflows");
    pairs_ = n_ * (n_ - (n_ != 0)) / 2;
}

std::size_t PairIndex::offset(std::size_t first) const noexcept {
    return first * (2 * n_ - first - 1) / 2;
}

std::size_t PairIndex::index(Graph::Vertex first, Graph::Vertex second) const {
    if (first >= second || second >= n_) throw std::out_of_range("invalid ordered vertex pair");
    return offset(first) + (second - first - 1);
}

std::pair<Graph::Vertex, Graph::Vertex> PairIndex::pair(std::size_t value) const {
    if (value >= pairs_) throw std::out_of_range("pair index out of range");
    std::size_t low = 0, high = n_ - 1;
    while (low + 1 < high) {
        const auto middle = low + (high - low) / 2;
        if (offset(middle) <= value) low = middle;
        else high = middle;
    }
    const std::size_t first = low;
    const std::size_t second = first + 1 + value - offset(first);
    return {static_cast<Graph::Vertex>(first), static_cast<Graph::Vertex>(second)};
}

std::size_t packed_size(std::size_t dimension) noexcept {
    return dimension * (dimension + 1) / 2;
}

std::size_t packed_index(std::size_t dimension, std::size_t row, std::size_t column) {
    if (row >= dimension || column >= dimension) throw std::out_of_range("packed matrix index");
    if (row > column) std::swap(row, column);
    return row * dimension - row * (row - (row != 0)) / 2 + (column - row);
}

CutwidthSdpOperator::CutwidthSdpOperator(const Graph& graph)
    : graph_(graph), pairs_(graph.size()), dimension_(pairs_.pair_count() + 1),
      vertex_terms_(graph.size()) {
    for (Graph::Vertex cut = 0; cut < graph.size(); ++cut) {
        auto& terms = vertex_terms_[cut];
        for (Graph::Vertex u = 0; u < graph.size(); ++u) {
            for (Graph::Vertex v = u + 1; v < graph.size(); ++v) {
                if (!graph.adjacent(u, v)) continue;
                const auto a = before_or_equal(u, cut);
                const auto b = before_or_equal(v, cut);
                // a + b - 2ab, with each indicator c+s*x.
                add_polynomial_term(terms, 0, 0,
                    a.constant + b.constant - 2 * a.constant * b.constant);
                if (a.slope != 0)
                    add_polynomial_term(terms, 0, a.coordinate,
                        a.slope - 2 * a.slope * b.constant);
                if (b.slope != 0)
                    add_polynomial_term(terms, 0, b.coordinate,
                        b.slope - 2 * b.slope * a.constant);
                if (a.slope != 0 && b.slope != 0)
                    add_polynomial_term(terms, a.coordinate, b.coordinate,
                        -2 * a.slope * b.slope);
            }
        }
        std::sort(terms.begin(), terms.end(), [](const Term& a, const Term& b) {
            return a.packed < b.packed;
        });
        std::vector<Term> combined;
        for (const auto term : terms) {
            if (!combined.empty() && combined.back().packed == term.packed)
                combined.back().coefficient += term.coefficient;
            else combined.push_back(term);
        }
        std::erase_if(combined, [](const Term& term) { return term.coefficient == 0.0; });
        terms = std::move(combined);
    }
}

CutwidthSdpOperator::AffineIndicator CutwidthSdpOperator::before_or_equal(
    Graph::Vertex endpoint, Graph::Vertex cut_vertex) const {
    if (endpoint == cut_vertex) return {1.0, 0.0, 0};
    if (endpoint < cut_vertex)
        return {0.0, 1.0, pairs_.coordinate(endpoint, cut_vertex)};
    return {1.0, -1.0, pairs_.coordinate(cut_vertex, endpoint)};
}

void CutwidthSdpOperator::add_polynomial_term(
    std::vector<Term>& terms, std::size_t row, std::size_t column,
    double coefficient) const {
    if (coefficient != 0.0)
        terms.push_back({packed_index(dimension_, row, column), coefficient});
}

std::size_t CutwidthSdpOperator::constraint_count() const noexcept {
    return 1 + pairs_.pair_count() + graph_.size();
}

double CutwidthSdpOperator::evaluate_vertex_cut(
    Graph::Vertex vertex, std::span<const double> moment) const {
    if (vertex >= graph_.size()) throw std::out_of_range("cut vertex out of range");
    if (moment.size() != packed_moment_size()) throw std::invalid_argument("wrong moment size");
    double result = 0.0;
    for (const auto term : vertex_terms_[vertex]) result += term.coefficient * moment[term.packed];
    return result;
}

void CutwidthSdpOperator::apply(std::span<const double> moment,
                                std::span<double> constraints) const {
    if (moment.size() != packed_moment_size() || constraints.size() != constraint_count())
        throw std::invalid_argument("SDP operator dimension mismatch");
    constraints[0] = moment[packed_index(dimension_, 0, 0)];
    for (std::size_t pair = 0; pair < pairs_.pair_count(); ++pair) {
        const auto coordinate = pair + 1;
        constraints[1 + pair] = moment[packed_index(dimension_, coordinate, coordinate)] -
                                moment[packed_index(dimension_, 0, coordinate)];
    }
    for (Graph::Vertex vertex = 0; vertex < graph_.size(); ++vertex)
        constraints[1 + pairs_.pair_count() + vertex] = evaluate_vertex_cut(vertex, moment);
}

void CutwidthSdpOperator::apply_adjoint(std::span<const double> multipliers,
                                        std::span<double> moment) const {
    if (moment.size() != packed_moment_size() || multipliers.size() != constraint_count())
        throw std::invalid_argument("SDP adjoint dimension mismatch");
    std::fill(moment.begin(), moment.end(), 0.0);
    moment[packed_index(dimension_, 0, 0)] += multipliers[0];
    for (std::size_t pair = 0; pair < pairs_.pair_count(); ++pair) {
        const auto coordinate = pair + 1;
        const double value = multipliers[1 + pair];
        moment[packed_index(dimension_, coordinate, coordinate)] += value;
        moment[packed_index(dimension_, 0, coordinate)] -= value;
    }
    for (Graph::Vertex vertex = 0; vertex < graph_.size(); ++vertex) {
        const double value = multipliers[1 + pairs_.pair_count() + vertex];
        for (const auto term : vertex_terms_[vertex])
            moment[term.packed] += value * term.coefficient;
    }
}

} // namespace cutwidth::sdp
