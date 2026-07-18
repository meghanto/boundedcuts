#pragma once

#include "graph.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace cutwidth::sdp {

class PairIndex {
public:
    explicit PairIndex(std::size_t vertex_count);
    [[nodiscard]] std::size_t vertex_count() const noexcept { return n_; }
    [[nodiscard]] std::size_t pair_count() const noexcept { return pairs_; }
    [[nodiscard]] std::size_t index(Graph::Vertex first, Graph::Vertex second) const;
    [[nodiscard]] std::pair<Graph::Vertex, Graph::Vertex> pair(std::size_t index) const;
    // Coordinate zero is the homogenizing constant.
    [[nodiscard]] std::size_t coordinate(Graph::Vertex first, Graph::Vertex second) const {
        return 1 + index(first, second);
    }

private:
    [[nodiscard]] std::size_t offset(std::size_t first) const noexcept;
    std::size_t n_ = 0;
    std::size_t pairs_ = 0;
};

// Packed upper-triangle indexing. Moment stencils store polynomial
// coefficients directly: an off-diagonal monomial coefficient q multiplies
// Z(i,j) once (rather than using the doubled Frobenius convention).
[[nodiscard]] std::size_t packed_index(std::size_t dimension,
                                       std::size_t row, std::size_t column);
[[nodiscard]] std::size_t packed_size(std::size_t dimension) noexcept;

class CutwidthSdpOperator {
public:
    explicit CutwidthSdpOperator(const Graph& graph);

    [[nodiscard]] std::size_t dimension() const noexcept { return dimension_; }
    [[nodiscard]] std::size_t packed_moment_size() const noexcept {
        return packed_size(dimension_);
    }
    // Constraint layout: Z00, then Zii-Z0i for every pair coordinate, then
    // one C_v moment value per graph vertex.
    [[nodiscard]] std::size_t constraint_count() const noexcept;

    void apply(std::span<const double> packed_moment,
               std::span<double> constraints) const;
    void apply_adjoint(std::span<const double> multipliers,
                       std::span<double> packed_moment) const;
    [[nodiscard]] double evaluate_vertex_cut(
        Graph::Vertex vertex, std::span<const double> packed_moment) const;

private:
    struct Term { std::size_t packed; double coefficient; };
    struct AffineIndicator {
        double constant = 0.0;
        double slope = 0.0;
        std::size_t coordinate = 0;
    };

    [[nodiscard]] AffineIndicator before_or_equal(
        Graph::Vertex endpoint, Graph::Vertex cut_vertex) const;
    void add_polynomial_term(std::vector<Term>& terms,
                             std::size_t row, std::size_t column,
                             double coefficient) const;

    const Graph& graph_;
    PairIndex pairs_;
    std::size_t dimension_;
    std::vector<std::vector<Term>> vertex_terms_;
};

} // namespace cutwidth::sdp
