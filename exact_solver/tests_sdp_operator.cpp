#include "sdp_operator.hpp"
#include "sdp_admm.hpp"
#include "sdp_certificate.hpp"
#include "clarabel_sdp_adapter.hpp"
#include "oracle.hpp"
#include "optimizer_v2.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
using cutwidth::Graph;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Graph graph_from_bits(std::uint32_t n, std::uint64_t bits) {
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    std::uint32_t edge = 0;
    for (Graph::Vertex u = 0; u < n; ++u)
        for (Graph::Vertex v = u + 1; v < n; ++v, ++edge)
            if ((bits >> edge) & 1U) edges.emplace_back(u, v);
    return Graph(n, edges);
}

std::vector<double> rank_one_moment(const Graph& graph,
                                    const std::vector<Graph::Vertex>& ordering) {
    cutwidth::sdp::PairIndex pairs(graph.size());
    const std::size_t dimension = pairs.pair_count() + 1;
    std::vector<std::size_t> position(graph.size());
    for (std::size_t p = 0; p < ordering.size(); ++p) position[ordering[p]] = p;
    std::vector<double> vector(dimension, 1.0);
    for (Graph::Vertex u = 0; u < graph.size(); ++u)
        for (Graph::Vertex v = u + 1; v < graph.size(); ++v)
            vector[pairs.coordinate(u, v)] = position[u] < position[v] ? 1.0 : 0.0;
    std::vector<double> moment(cutwidth::sdp::packed_size(dimension));
    for (std::size_t i = 0; i < dimension; ++i)
        for (std::size_t j = i; j < dimension; ++j)
            moment[cutwidth::sdp::packed_index(dimension, i, j)] = vector[i] * vector[j];
    return moment;
}

std::uint32_t cut_after_vertex(const Graph& graph,
                               const std::vector<Graph::Vertex>& ordering,
                               Graph::Vertex cut_vertex) {
    std::vector<bool> prefix(graph.size(), false);
    for (const auto vertex : ordering) {
        prefix[vertex] = true;
        if (vertex == cut_vertex) break;
    }
    std::uint32_t result = 0;
    for (Graph::Vertex u = 0; u < graph.size(); ++u)
        for (Graph::Vertex v = u + 1; v < graph.size(); ++v)
            if (graph.adjacent(u, v) && prefix[u] != prefix[v]) ++result;
    return result;
}

void pair_index_tests() {
    for (std::size_t n = 0; n <= 30; ++n) {
        cutwidth::sdp::PairIndex pairs(n);
        std::size_t expected = 0;
        for (Graph::Vertex u = 0; u < n; ++u)
            for (Graph::Vertex v = u + 1; v < n; ++v, ++expected) {
                require(pairs.index(u, v) == expected, "pair index is not lexicographic");
                require(pairs.pair(expected) == std::pair{u, v}, "pair inverse mismatch");
            }
        require(expected == pairs.pair_count(), "pair count mismatch");
    }
}

void exhaustive_rank_one_tests() {
    for (std::uint32_t n = 0; n <= 5; ++n) {
        const auto pair_count = n * (n - 1) / 2;
        const std::uint64_t graph_count = std::uint64_t{1} << pair_count;
        for (std::uint64_t bits = 0; bits < graph_count; ++bits) {
            const auto graph = graph_from_bits(n, bits);
            const cutwidth::sdp::CutwidthSdpOperator op(graph);
            std::vector<Graph::Vertex> ordering(n);
            std::iota(ordering.begin(), ordering.end(), Graph::Vertex{0});
            do {
                const auto moment = rank_one_moment(graph, ordering);
                std::vector<double> constraints(op.constraint_count());
                op.apply(moment, constraints);
                require(constraints[0] == 1.0, "rank-one normalization mismatch");
                const cutwidth::sdp::PairIndex pairs(n);
                for (std::size_t pair = 0; pair < pairs.pair_count(); ++pair)
                    require(constraints[1 + pair] == 0.0, "rank-one diagonal mismatch");
                for (Graph::Vertex vertex = 0; vertex < n; ++vertex) {
                    const auto expected = cut_after_vertex(graph, ordering, vertex);
                    const auto actual = op.evaluate_vertex_cut(vertex, moment);
                    require(actual == expected, "C_v rank-one value differs from cut");
                    require(constraints[1 + pairs.pair_count() + vertex] == expected,
                            "operator C_v output differs from cut");
                }
            } while (std::next_permutation(ordering.begin(), ordering.end()));
        }
    }
}

void randomized_adjoint_tests() {
    std::mt19937_64 rng(0x5D0AD101ULL);
    std::uniform_real_distribution<double> real(-1.0, 1.0);
    for (std::uint32_t n = 2; n <= 14; ++n) {
        for (unsigned sample = 0; sample < 20; ++sample) {
            std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
            for (Graph::Vertex u = 0; u < n; ++u)
                for (Graph::Vertex v = u + 1; v < n; ++v)
                    if ((rng() & 3U) == 0) edges.emplace_back(u, v);
            const Graph graph(n, edges);
            const cutwidth::sdp::CutwidthSdpOperator op(graph);
            std::vector<double> moment(op.packed_moment_size());
            std::vector<double> dual(op.constraint_count());
            for (auto& value : moment) value = real(rng);
            for (auto& value : dual) value = real(rng);
            std::vector<double> applied(op.constraint_count());
            std::vector<double> adjoint(op.packed_moment_size());
            op.apply(moment, applied);
            op.apply_adjoint(dual, adjoint);
            const double lhs = std::inner_product(applied.begin(), applied.end(), dual.begin(), 0.0);
            const double rhs = std::inner_product(moment.begin(), moment.end(), adjoint.begin(), 0.0);
            const double scale = 1.0 + std::abs(lhs) + std::abs(rhs);
            require(std::abs(lhs - rhs) <= 2e-13 * scale, "SDP operator adjoint identity failed");
        }
    }
}

void dense_admm_smoke_test() {
    const Graph triangle(3, {{0,1},{0,2},{1,2}});
    const cutwidth::sdp::CutwidthSdpOperator op(triangle);
    cutwidth::sdp::DenseAdmmOptions options;
    options.alpha = 2.0;
    options.iterations = 1000;
    options.constraint_projection_sweeps = 5;
    options.tolerance = 1e-7;
    const auto result = cutwidth::sdp::solve_dense_feasibility_admm(op, options);
    require(std::isfinite(result.primal_residual) &&
            std::isfinite(result.equality_residual) &&
            std::isfinite(result.cut_violation),
            "dense ADMM returned non-finite diagnostics");
    require(result.primal_residual < 1e-5 && result.equality_residual < 1e-5 &&
            result.cut_violation < 1e-5,
            "dense ADMM did not recover feasible K3 relaxation point");
}

void exhaustive_certificate_tests() {
    std::mt19937_64 rng(0xCE471F1CULL);
    std::uniform_real_distribution<double> real(-4.0, 4.0);
    for (std::uint32_t n = 1; n <= 5; ++n) {
        const auto pair_count = n * (n - 1) / 2;
        const std::uint64_t graph_count = std::uint64_t{1} << pair_count;
        for (std::uint64_t bits = 0; bits < graph_count; ++bits) {
            const auto graph = graph_from_bits(n, bits);
            const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
            const cutwidth::sdp::CutwidthSdpOperator op(graph);
            for (unsigned sample = 0; sample < 4; ++sample) {
                cutwidth::sdp::BasicDualCandidate candidate;
                candidate.normalization_multiplier = real(rng);
                candidate.diagonal_multipliers.resize(pair_count);
                candidate.cut_weights.resize(n);
                for (auto& value : candidate.diagonal_multipliers) value = real(rng);
                for (auto& value : candidate.cut_weights) value = std::abs(real(rng));
                const auto certificate = cutwidth::sdp::recover_basic_certificate(op, candidate);
                require(certificate.valid && certificate.integer_lower_bound.has_value(),
                        "finite dual candidate failed certificate recovery");
                require(*certificate.integer_lower_bound <= optimum,
                        "recovered SDP certificate exceeds exact optimum");
            }
            if (n <= 3) {
                cutwidth::OptimizerV2Options options;
                options.sdp_iterations = 5;
                options.sdp_max_dimension = 32;
                const auto integrated = cutwidth::optimize_cutwidth_v2(graph, options);
                require(integrated.optimal && integrated.upper_bound == optimum &&
                        integrated.stats.sdp_attempted && integrated.stats.sdp_available,
                        "root SDP integration changed exact optimization result");
                require(integrated.stats.sdp_certified_lower_bound.has_value() &&
                        *integrated.stats.sdp_certified_lower_bound <= optimum,
                        "root integration consumed an invalid SDP certificate");
            }
        }
    }

    const Graph edge(2, {{0,1}});
    const cutwidth::sdp::CutwidthSdpOperator op(edge);
    cutwidth::sdp::BasicDualCandidate adversarial;
    adversarial.diagonal_multipliers = {0.0};
    adversarial.cut_weights = {1.0, 1.0};
    adversarial.normalization_multiplier = std::numeric_limits<double>::quiet_NaN();
    require(!cutwidth::sdp::recover_basic_certificate(op, adversarial).valid,
            "NaN dual candidate produced a certificate");
    adversarial.normalization_multiplier = 0.0;
    adversarial.cut_weights[0] = std::numeric_limits<double>::infinity();
    require(!cutwidth::sdp::recover_basic_certificate(op, adversarial).valid,
            "infinite dual candidate produced a certificate");
    adversarial.cut_weights = {0.0, 0.0};
    require(!cutwidth::sdp::recover_basic_certificate(op, adversarial).valid,
            "zero simplex weights produced a certificate");
    adversarial.cut_weights = {1.0, 1.0};
    adversarial.diagonal_multipliers.clear();
    require(!cutwidth::sdp::recover_basic_certificate(op, adversarial).valid,
            "wrong dual dimension produced a certificate");
}

void exhaustive_bisection_identity_and_certificate_tests() {
    for (std::uint32_t n = 2; n <= 6; ++n) {
        const auto pair_count = n * (n - 1) / 2;
        const std::uint64_t graph_count = std::uint64_t{1} << pair_count;
        for (std::uint64_t bits = 0; bits < graph_count; ++bits) {
            const auto graph = graph_from_bits(n, bits);
            const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
            for (std::uint64_t subset = 0; subset < (std::uint64_t{1} << n); ++subset) {
                std::uint32_t cut = 0;
                double quadratic = 0.0;
                for (Graph::Vertex u = 0; u < n; ++u) {
                    const double x_u = ((subset >> u) & 1U) ? 1.0 : -1.0;
                    for (Graph::Vertex v = u + 1; v < n; ++v) {
                        if (!graph.adjacent(u, v)) continue;
                        const double x_v = ((subset >> v) & 1U) ? 1.0 : -1.0;
                        quadratic += 0.25 * (x_u - x_v) * (x_u - x_v);
                        cut += x_u != x_v;
                    }
                }
                require(quadratic == cut, "bisection rank-one objective differs from cut");
            }
            for (std::size_t cardinality = 1; cardinality < n; ++cardinality) {
                std::uint32_t exact_fixed = std::numeric_limits<std::uint32_t>::max();
                for (std::uint64_t subset = 0; subset < (std::uint64_t{1} << n); ++subset)
                    if (std::popcount(subset) == cardinality)
                        exact_fixed = std::min(exact_fixed, graph.cut(subset));
                std::vector<double> y(n);
                for (std::size_t i = 0; i < n; ++i)
                    y[i] = (static_cast<int>((bits + i) % 9) - 4) / 16.0;
                const double z = (static_cast<int>(bits % 7) - 3) / 32.0;
                const auto certificate = cutwidth::sdp::recover_bisection_certificate(
                    graph, cardinality, y, z, 30);
                require(certificate.valid && certificate.integer_lower_bound.has_value(),
                        "finite bisection dual failed certificate recovery");
                require(cutwidth::sdp::verify_bisection_certificate(graph, certificate),
                        "recovered bisection certificate failed exact verification");
                require(*certificate.integer_lower_bound <= exact_fixed &&
                        *certificate.integer_lower_bound <= optimum,
                        "bisection SDP certificate exceeds exact fixed-cardinality cut");
                if (n >= 3) {
                    const std::vector<cutwidth::sdp::BisectionTriangleCut> cuts{
                        {0, 1, 2, 0}, {0, 1, 2, 1},
                        {0, 1, 2, 2}, {0, 1, 2, 3}};
                    const std::vector<double> multipliers{0.125, 0.25, 0.375, 0.5};
                    const auto triangle_certificate =
                        cutwidth::sdp::recover_bisection_certificate(
                            graph, cardinality, y, z, 30, cuts, multipliers);
                    require(triangle_certificate.valid &&
                            triangle_certificate.integer_lower_bound &&
                            cutwidth::sdp::verify_bisection_certificate(
                                graph, triangle_certificate) &&
                            *triangle_certificate.integer_lower_bound <= exact_fixed &&
                            *triangle_certificate.integer_lower_bound <= optimum,
                            "triangle certificate exceeds exact fixed-cardinality cut");
                    auto tampered = triangle_certificate;
                    tampered.triangle_multipliers.front().numerator = -1;
                    require(!cutwidth::sdp::verify_bisection_certificate(graph, tampered),
                            "negative triangle multiplier was accepted");
                }
            }
        }
    }

    const Graph edge(2, {{0, 1}});
    const std::vector<double> finite_y(2, 0.0);
    auto bad_y = finite_y;
    bad_y[0] = std::numeric_limits<double>::quiet_NaN();
    require(!cutwidth::sdp::recover_bisection_certificate(edge, 1, bad_y, 0.0, 30).valid,
            "NaN bisection multiplier produced a certificate");
    require(!cutwidth::sdp::recover_bisection_certificate(
                edge, 1, finite_y, std::numeric_limits<double>::infinity(), 30).valid,
            "infinite bisection multiplier produced a certificate");
    require(!cutwidth::sdp::recover_bisection_certificate(edge, 0, finite_y, 0.0, 30).valid,
            "invalid bisection cardinality produced a certificate");
}

void weighted_bisection_identity_certificate_and_tamper_tests() {
    // Exercise nonuniform boundary terms independently of Clarabel.  Every
    // rank-one point must equal the combinatorial residual cut exactly, and an
    // arbitrary finite dual repaired by exact arithmetic must remain below the
    // fixed-cardinality minimum.
    for (std::uint32_t n = 2; n <= 5; ++n) {
        const auto pair_count = n * (n - 1) / 2;
        const std::uint64_t graph_count = std::uint64_t{1} << pair_count;
        for (std::uint64_t bits = 0; bits < graph_count; ++bits) {
            const auto graph = graph_from_bits(n, bits);
            std::vector<std::uint32_t> boundary(n);
            std::uint32_t boundary_sum = 0;
            for (std::size_t i = 0; i < n; ++i) {
                boundary[i] = static_cast<std::uint32_t>((bits + 3 * i + 1) % 5);
                boundary_sum += boundary[i];
            }
            for (std::size_t cardinality = 1; cardinality < n; ++cardinality) {
                std::uint32_t exact_min = std::numeric_limits<std::uint32_t>::max();
                for (std::uint64_t subset = 0; subset < (std::uint64_t{1} << n); ++subset) {
                    if (std::popcount(subset) != cardinality) continue;
                    std::uint32_t combinatorial = 0;
                    double polynomial = static_cast<double>(boundary_sum) / 2.0;
                    for (Graph::Vertex i = 0; i < n; ++i) {
                        const double x_i = ((subset >> i) & 1U) ? 1.0 : -1.0;
                        polynomial -= 0.5 * boundary[i] * x_i;
                        if (((subset >> i) & 1U) == 0) combinatorial += boundary[i];
                        for (Graph::Vertex j = i + 1; j < n; ++j) {
                            if (!graph.adjacent(i, j)) continue;
                            const double x_j = ((subset >> j) & 1U) ? 1.0 : -1.0;
                            polynomial += 0.25 * (x_i - x_j) * (x_i - x_j);
                            combinatorial += x_i != x_j;
                        }
                    }
                    require(polynomial == combinatorial,
                            "weighted bisection rank-one objective differs from residual cut");
                    exact_min = std::min(exact_min, combinatorial);
                }
                std::vector<double> y(n);
                for (std::size_t i = 0; i < n; ++i)
                    y[i] = (static_cast<int>((bits + i) % 7) - 3) / 16.0;
                const auto certificate = cutwidth::sdp::recover_weighted_bisection_certificate(
                    graph, boundary, cardinality, 0.125, y, -0.0625, 0.03125, 30);
                require(certificate.valid && certificate.integer_lower_bound,
                        "finite weighted dual failed exact certificate recovery");
                require(cutwidth::sdp::verify_weighted_bisection_certificate(
                            graph, boundary, certificate),
                        "weighted certificate failed exact verification");
                require(*certificate.integer_lower_bound <= exact_min,
                        "weighted certificate exceeds exact fixed-cardinality minimum");
            }
        }
    }

    const Graph path(3, {{0, 1}, {1, 2}});
    const std::vector<std::uint32_t> boundary{1, 3, 2};
    const std::vector<double> y(3, 0.0);
    const auto original = cutwidth::sdp::recover_weighted_bisection_certificate(
        path, boundary, 1, 0.0, y, 0.0, 0.0, 30);
    require(original.valid, "weighted tamper fixture certificate is invalid");
    auto tampered = original;
    ++tampered.objective_numerator;
    require(!cutwidth::sdp::verify_weighted_bisection_certificate(path, boundary, tampered),
            "tampered weighted objective was accepted");
    tampered = original;
    ++tampered.model_hash;
    require(!cutwidth::sdp::verify_weighted_bisection_certificate(path, boundary, tampered),
            "tampered weighted model hash was accepted");
    auto changed_boundary = boundary;
    ++changed_boundary[0];
    require(!cutwidth::sdp::verify_weighted_bisection_certificate(
                path, changed_boundary, original),
            "weighted certificate was accepted for different boundary weights");
    tampered = original;
    tampered.diagonal_repair_numerators.pop_back();
    require(!cutwidth::sdp::verify_weighted_bisection_certificate(path, boundary, tampered),
            "weighted certificate with wrong repair dimension was accepted");
}

#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
void clarabel_small_graph_tests() {
    for (std::uint32_t n = 1; n <= 3; ++n) {
        const auto pairs = n * (n - 1) / 2;
        for (std::uint64_t bits = 0; bits < (std::uint64_t{1} << pairs); ++bits) {
            const auto graph = graph_from_bits(n, bits);
            const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
            const cutwidth::sdp::CutwidthSdpOperator op(graph);
            cutwidth::sdp::ClarabelSdpOptions options;
            options.max_dimension = 32;
            options.max_iterations = 200;
            options.time_limit_seconds = 5.0;
            const auto result = cutwidth::sdp::solve_basic_sdp_clarabel(op, options);
            require(result.status == cutwidth::sdp::ClarabelSdpStatus::solved ||
                    result.status == cutwidth::sdp::ClarabelSdpStatus::almost_solved,
                    "Clarabel failed on exhaustive small graph");
            require(std::isfinite(result.primal_objective) &&
                    result.primal_objective <= static_cast<double>(optimum) + 1e-5,
                    "Clarabel basic SDP objective exceeds rank-one optimum");
            require(result.certificate.valid && result.certificate.integer_lower_bound &&
                    *result.certificate.integer_lower_bound <= optimum,
                    "Clarabel mapped dual produced invalid certificate");
            if (n >= 2) {
                cutwidth::sdp::ClarabelBisectionOptions bisection_options;
                bisection_options.cardinality = n / 2;
                bisection_options.max_iterations = 200;
                bisection_options.time_limit_seconds = 5.0;
                const auto bisection = cutwidth::sdp::solve_bisection_sdp_clarabel(
                    graph, bisection_options);
                require(bisection.status == cutwidth::sdp::ClarabelSdpStatus::solved ||
                        bisection.status == cutwidth::sdp::ClarabelSdpStatus::almost_solved,
                        "Clarabel compact bisection SDP failed on exhaustive small graph");
                require(bisection.certificate.valid &&
                        bisection.certificate.integer_lower_bound &&
                        cutwidth::sdp::verify_bisection_certificate(
                            graph, bisection.certificate) &&
                        *bisection.certificate.integer_lower_bound <= optimum,
                        "Clarabel compact bisection certificate exceeds exact optimum");

                std::vector<std::uint32_t> boundary(n);
                for (std::size_t i = 0; i < n; ++i) boundary[i] = i + 1;
                cutwidth::sdp::ClarabelWeightedBisectionOptions weighted_options;
                weighted_options.cardinality = n / 2;
                weighted_options.max_iterations = 200;
                weighted_options.time_limit_seconds = 5.0;
                const auto weighted = cutwidth::sdp::solve_weighted_bisection_sdp_clarabel(
                    graph, boundary, weighted_options);
                require(weighted.status == cutwidth::sdp::ClarabelSdpStatus::solved ||
                        weighted.status == cutwidth::sdp::ClarabelSdpStatus::almost_solved,
                        "Clarabel weighted bisection SDP failed on exhaustive small graph");
                std::uint32_t weighted_exact = std::numeric_limits<std::uint32_t>::max();
                for (std::uint64_t subset = 0; subset < (std::uint64_t{1} << n); ++subset) {
                    if (std::popcount(subset) != weighted_options.cardinality) continue;
                    std::uint32_t value = 0;
                    for (Graph::Vertex i = 0; i < n; ++i) {
                        if (((subset >> i) & 1U) == 0) value += boundary[i];
                        for (Graph::Vertex j = i + 1; j < n; ++j)
                            if (graph.adjacent(i, j) &&
                                (((subset >> i) & 1U) != ((subset >> j) & 1U)))
                                ++value;
                    }
                    weighted_exact = std::min(weighted_exact, value);
                }
                require(weighted.certificate.valid && weighted.certificate.integer_lower_bound &&
                        cutwidth::sdp::verify_weighted_bisection_certificate(
                            graph, boundary, weighted.certificate) &&
                        *weighted.certificate.integer_lower_bound <= weighted_exact,
                        "Clarabel weighted bisection certificate exceeds exact fixed-k value");
            }
        }
    }
    const Graph cycle5(5, {{0,1},{1,2},{2,3},{3,4},{4,0}});
    cutwidth::sdp::ClarabelBisectionOptions triangle_options;
    triangle_options.cardinality = 2;
    triangle_options.max_iterations = 200;
    triangle_options.time_limit_seconds = 5.0;
    triangle_options.triangle_cut_limit = 64;
    const auto triangle = cutwidth::sdp::solve_bisection_sdp_clarabel(
        cycle5, triangle_options);
    require(triangle.triangle_cuts != 0 && triangle.certificate.valid &&
            triangle.certificate.integer_lower_bound &&
            cutwidth::sdp::verify_bisection_certificate(cycle5, triangle.certificate) &&
            *triangle.certificate.integer_lower_bound <= 2,
            "Clarabel triangle separation failed exact C5 certification");
}
#endif
}

int main() {
    try {
        pair_index_tests();
        exhaustive_rank_one_tests();
        randomized_adjoint_tests();
        dense_admm_smoke_test();
        exhaustive_certificate_tests();
        exhaustive_bisection_identity_and_certificate_tests();
        weighted_bisection_identity_certificate_and_tamper_tests();
#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
        clarabel_small_graph_tests();
#endif
        std::cout << "All SDP operator prototype tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SDP OPERATOR TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
