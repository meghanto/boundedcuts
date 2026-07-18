#include "clarabel_sdp_adapter.hpp"

#include <cmath>
#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>

#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
#include <clarabel.hpp>
#include <Eigen/Eigen>
#endif

namespace cutwidth::sdp {

#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
namespace {
struct DeadlineCallback {
    std::chrono::steady_clock::time_point deadline;
    bool expired = false;
};
int stop_at_deadline(clarabel::DefaultInfo<double>&, void* opaque) {
    auto& state = *static_cast<DeadlineCallback*>(opaque);
    state.expired = std::chrono::steady_clock::now() >= state.deadline;
    return state.expired ? 1 : 0;
}
} // namespace
#endif

ClarabelSdpResult solve_basic_sdp_clarabel(
    const CutwidthSdpOperator& op, const ClarabelSdpOptions& options) {
    ClarabelSdpResult result;
#ifndef CUTWIDTH_HAVE_CLARABEL_SDP
    (void)op;
    (void)options;
    result.diagnostic = "Clarabel.cpp SDP backend was not configured";
    return result;
#else
    const auto wall_started = std::chrono::steady_clock::now();
    const std::size_t d = op.dimension();
    if (d > options.max_dimension || op.packed_moment_size() > options.max_psd_triangle_entries) {
        result.status = ClarabelSdpStatus::unsupported;
        result.diagnostic = "lifted PSD cone exceeds configured dimension or entry cap";
        return result;
    }
    if (options.max_iterations > std::numeric_limits<std::uint32_t>::max() ||
        !std::isfinite(options.time_limit_seconds) || options.time_limit_seconds < 0)
        throw std::invalid_argument("invalid Clarabel SDP options");
    const std::size_t packed = op.packed_moment_size();
    const std::size_t pairs = d - 1;
    const std::size_t vertices = op.constraint_count() - 1 - pairs;
    const std::size_t variables = 1 + packed; // alpha, then raw packed moment
    const std::size_t psd_rows = packed;
    const std::size_t zero_rows = 1 + pairs;
    const std::size_t rows = psd_rows + zero_rows + vertices;
    using Triplet = Eigen::Triplet<double>;
    std::vector<Triplet> triplets;
    triplets.reserve(psd_rows + 3*pairs + vertices*packed/8);
    std::size_t cone_row = 0;
    const double sqrt2 = std::sqrt(2.0);
    // Clarabel PSDTriangle uses upper-triangle column order
    // (00,01,11,02,12,22,...) and sqrt(2) off-diagonal scaling.
    for (std::size_t column = 0; column < d; ++column) {
        for (std::size_t row = 0; row <= column; ++row, ++cone_row) {
            const double scale = row == column ? 1.0 : sqrt2;
            triplets.emplace_back(static_cast<Eigen::Index>(cone_row),
                                  static_cast<Eigen::Index>(1 + packed_index(d, column, row)),
                                  -scale);
        }
    }
    const std::size_t zero_offset = psd_rows;
    triplets.emplace_back(static_cast<Eigen::Index>(zero_offset),
                          static_cast<Eigen::Index>(1 + packed_index(d, 0, 0)), 1.0);
    for (std::size_t pair = 0; pair < pairs; ++pair) {
        const auto coordinate = pair + 1;
        const auto row = zero_offset + 1 + pair;
        triplets.emplace_back(static_cast<Eigen::Index>(row),
            static_cast<Eigen::Index>(1 + packed_index(d, coordinate, coordinate)), 1.0);
        triplets.emplace_back(static_cast<Eigen::Index>(row),
            static_cast<Eigen::Index>(1 + packed_index(d, 0, coordinate)), -1.0);
    }
    const std::size_t cut_offset = zero_offset + zero_rows;
    std::vector<double> multiplier(op.constraint_count(), 0.0), stencil(packed);
    for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
        multiplier[1 + pairs + vertex] = 1.0;
        op.apply_adjoint(multiplier, stencil);
        multiplier[1 + pairs + vertex] = 0.0;
        const auto row = cut_offset + vertex;
        triplets.emplace_back(static_cast<Eigen::Index>(row), 0, -1.0);
        for (std::size_t entry = 0; entry < packed; ++entry)
            if (stencil[entry] != 0.0)
                triplets.emplace_back(static_cast<Eigen::Index>(row),
                                      static_cast<Eigen::Index>(1 + entry), stencil[entry]);
    }
    Eigen::SparseMatrix<double> P(static_cast<Eigen::Index>(variables),
                                  static_cast<Eigen::Index>(variables));
    Eigen::VectorXd q = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(variables));
    q[0] = 1.0;
    Eigen::SparseMatrix<double> A(static_cast<Eigen::Index>(rows),
                                  static_cast<Eigen::Index>(variables));
    A.setFromTriplets(triplets.begin(), triplets.end());
    A.makeCompressed();
    Eigen::VectorXd b = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(rows));
    b[static_cast<Eigen::Index>(zero_offset)] = 1.0;
    std::vector<clarabel::SupportedConeT<double>> cones{
        clarabel::PSDTriangleConeT<double>(d),
        clarabel::ZeroConeT<double>(zero_rows),
        clarabel::NonnegativeConeT<double>(vertices)};
    auto settings = clarabel::DefaultSettings<double>::default_settings();
    settings.verbose = false;
    settings.max_iter = static_cast<std::uint32_t>(options.max_iterations);
    double remaining = options.time_limit_seconds;
    if (options.time_limit_seconds > 0) {
        const double build_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wall_started).count();
        remaining = options.time_limit_seconds - build_seconds;
        if (remaining <= 0) {
            result.status = ClarabelSdpStatus::max_time;
            result.diagnostic = "Clarabel budget exhausted during model construction";
            return result;
        }
        settings.time_limit = std::max(0.001, remaining - 0.25);
    }
    settings.chordal_decomposition_enable = options.chordal_decomposition;
    settings.chordal_decomposition_complete_dual = true;
    clarabel::DefaultSolver<double> solver(P, q, A, b, cones, settings);
    DeadlineCallback callback{std::chrono::steady_clock::time_point::max(), false};
    if (options.time_limit_seconds > 0) {
        callback.deadline = wall_started + std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(std::max(0.001,
                    options.time_limit_seconds - 0.25)));
        solver.set_termination_callback(stop_at_deadline, &callback);
    }
    solver.solve();
    if (options.time_limit_seconds > 0) solver.unset_termination_callback();
    const auto solution = solver.solution();
    using clarabel::SolverStatus;
    switch (solution.status) {
    case SolverStatus::Solved: result.status = ClarabelSdpStatus::solved; break;
    case SolverStatus::AlmostSolved: result.status = ClarabelSdpStatus::almost_solved; break;
    case SolverStatus::MaxIterations: result.status = ClarabelSdpStatus::max_iterations; break;
    case SolverStatus::MaxTime: result.status = ClarabelSdpStatus::max_time; break;
    case SolverStatus::PrimalInfeasible:
    case SolverStatus::AlmostPrimalInfeasible: result.status = ClarabelSdpStatus::infeasible; break;
    case SolverStatus::NumericalError: result.status = ClarabelSdpStatus::numerical_error; break;
    case SolverStatus::CallbackTerminated:
        result.status = callback.expired ? ClarabelSdpStatus::max_time
                                         : ClarabelSdpStatus::unknown;
        break;
    default: result.status = ClarabelSdpStatus::unknown; break;
    }
    result.primal_objective = solution.obj_val;
    result.dual_objective = solution.obj_val_dual;
    result.primal_residual = solution.r_prim;
    result.dual_residual = solution.r_dual;
    result.solve_seconds = solution.solve_time;
    result.iterations = solution.iterations;
    if (solution.x.size() == static_cast<Eigen::Index>(variables)) {
        result.packed_moment.assign(solution.x.data() + 1,
                                    solution.x.data() + static_cast<Eigen::Index>(variables));
    }
    if (solution.z.size() == static_cast<Eigen::Index>(rows)) {
        result.dual_candidate.normalization_multiplier = solution.z[zero_offset];
        result.dual_candidate.diagonal_multipliers.resize(pairs);
        for (std::size_t pair = 0; pair < pairs; ++pair)
            result.dual_candidate.diagonal_multipliers[pair] = solution.z[zero_offset + 1 + pair];
        result.dual_candidate.cut_weights.resize(vertices);
        for (std::size_t vertex = 0; vertex < vertices; ++vertex)
            result.dual_candidate.cut_weights[vertex] = solution.z[cut_offset + vertex];
        result.certificate = recover_basic_certificate(
            op, result.dual_candidate, {options.max_dimension});
    }
    return result;
#endif
}

namespace {
using boost::multiprecision::cpp_int;

cpp_int abs_int(const cpp_int& value) { return value < 0 ? -value : value; }

bool quantize_dyadic(double value, unsigned bits, cpp_int& output) {
    if (!std::isfinite(value) || bits > 50) return false;
    const long double scaled = std::ldexp(static_cast<long double>(value),
                                          static_cast<int>(bits));
    if (!std::isfinite(scaled) ||
        scaled < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        scaled > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        return false;
    output = static_cast<std::int64_t>(std::llround(scaled));
    return true;
}

std::optional<std::uint32_t> nonnegative_ceiling(const cpp_int& numerator,
                                                  const cpp_int& denominator) {
    if (numerator <= 0) return std::uint32_t{0};
    const cpp_int value = (numerator + denominator - 1) / denominator;
    if (value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return value.convert_to<std::uint32_t>();
}

using IntegerMatrix = std::vector<std::vector<cpp_int>>;

IntegerMatrix bisection_slack_numerator(
    const Graph& graph, const std::vector<cpp_int>& y,
    const cpp_int& z, const cpp_int& denominator,
    std::span<const BisectionTriangleMultiplier> triangles = {}) {
    const std::size_t n = graph.size();
    IntegerMatrix matrix(n, std::vector<cpp_int>(n));
    const cpp_int quarter = denominator / 4;
    for (std::size_t i = 0; i < n; ++i) {
        matrix[i][i] = quarter * graph.degree(static_cast<Graph::Vertex>(i))
                     - y[i] - z;
        for (std::size_t j = i + 1; j < n; ++j) {
            cpp_int entry = -z;
            if (graph.adjacent(static_cast<Graph::Vertex>(i),
                               static_cast<Graph::Vertex>(j)))
                entry -= quarter;
            matrix[i][j] = entry;
            matrix[j][i] = entry;
        }
    }
    constexpr int signs[4][3] = {
        {1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
    for (const auto& triangle : triangles) {
        const auto& cut = triangle.cut;
        if (!(cut.a < cut.b && cut.b < cut.c && cut.c < n && cut.form < 4))
            return {};
        const std::size_t ends[3][2] = {
            {cut.a, cut.b}, {cut.a, cut.c}, {cut.b, cut.c}};
        for (std::size_t pair = 0; pair < 3; ++pair) {
            // The correlation coefficient in the inequality is represented
            // by symmetric matrix entries sign/2 under the Frobenius product.
            const cpp_int contribution =
                triangle.numerator * signs[cut.form][pair];
            if ((contribution & 1) != 0) {
                // All production denominators have at least two dyadic bits;
                // doubling the multiplier denominator is unnecessary when
                // quantization produced an odd numerator, so retain exactness
                // by rejecting this candidate and falling back to no bound.
                return {};
            }
            const auto i = ends[pair][0], j = ends[pair][1];
            matrix[i][j] -= contribution / 2;
            matrix[j][i] = matrix[i][j];
        }
    }
    return matrix;
}

// Fraction-free symmetric elimination.  When all preceding leading principal
// minors are positive, the current diagonal is the next leading principal
// minor.  Thus positivity of every pivot is exactly Sylvester's criterion.
// nullopt denotes a failed exact division, never an approximate decision.
std::optional<bool> strictly_positive_definite(
    const IntegerMatrix& input, const cpp_int& diagonal_shift) {
    if (diagonal_shift < 0 || input.empty()) return false;
    IntegerMatrix matrix = input;
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        if (matrix[i].size() != matrix.size()) return std::nullopt;
        matrix[i][i] += diagonal_shift;
    }
    cpp_int previous = 1;
    for (std::size_t k = 0; k < matrix.size(); ++k) {
        const cpp_int pivot = matrix[k][k];
        if (pivot <= 0) return false;
        for (std::size_t i = k + 1; i < matrix.size(); ++i) {
            for (std::size_t j = i; j < matrix.size(); ++j) {
                const cpp_int numerator = pivot * matrix[i][j]
                                        - matrix[i][k] * matrix[k][j];
                const cpp_int remainder = numerator % previous;
                if (remainder != 0) return std::nullopt;
                matrix[i][j] = numerator / previous;
                matrix[j][i] = matrix[i][j];
            }
        }
        previous = pivot;
    }
    return true;
}

std::optional<cpp_int> minimum_strict_shift(const IntegerMatrix& matrix) {
    if (matrix.empty()) return std::nullopt;
    cpp_int minimum_margin;
    bool first = true;
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        cpp_int margin = matrix[i][i];
        for (std::size_t j = 0; j < matrix.size(); ++j)
            if (i != j) margin -= abs_int(matrix[i][j]);
        if (first || margin < minimum_margin) minimum_margin = margin;
        first = false;
    }
    cpp_int high = minimum_margin < 0 ? -minimum_margin + 1 : cpp_int(1);
    const auto zero_ok = strictly_positive_definite(matrix, 0);
    if (!zero_ok) return std::nullopt;
    if (*zero_ok) return cpp_int(0);
    const auto high_ok = strictly_positive_definite(matrix, high);
    if (!high_ok || !*high_ok) return std::nullopt;
    cpp_int low = 0;
    while (low + 1 < high) {
        const cpp_int middle = (low + high) / 2;
        const auto middle_ok = strictly_positive_definite(matrix, middle);
        if (!middle_ok) return std::nullopt;
        if (*middle_ok) high = middle;
        else low = middle;
    }
    return high;
}
} // namespace

BisectionCertificate recover_bisection_certificate(
    const Graph& graph, std::size_t cardinality,
    std::span<const double> y, double z, unsigned fractional_bits,
    std::span<const BisectionTriangleCut> triangle_cuts,
    std::span<const double> triangle_multipliers) {
    BisectionCertificate certificate;
    certificate.vertex_count = graph.size();
    certificate.cardinality = cardinality;
    certificate.fractional_bits = fractional_bits;
    if (graph.size() == 0 || cardinality == 0 || cardinality >= graph.size() ||
        y.size() != graph.size() || fractional_bits < 2 || fractional_bits > 50 ||
        triangle_cuts.size() != triangle_multipliers.size())
        return certificate;

    certificate.denominator = cpp_int(1) << fractional_bits;
    certificate.y_numerators.resize(graph.size());
    if (!quantize_dyadic(z, fractional_bits, certificate.z_numerator)) return certificate;
    for (std::size_t i = 0; i < graph.size(); ++i)
        if (!quantize_dyadic(y[i], fractional_bits, certificate.y_numerators[i]))
            return certificate;
    certificate.triangle_multipliers.reserve(triangle_cuts.size());
    for (std::size_t i = 0; i < triangle_cuts.size(); ++i) {
        const auto& cut = triangle_cuts[i];
        if (!(cut.a < cut.b && cut.b < cut.c && cut.c < graph.size() && cut.form < 4) ||
            !std::isfinite(triangle_multipliers[i]))
            return certificate;
        BisectionTriangleMultiplier exact;
        exact.cut = cut;
        // Use the next coarser grid so division by two in the symmetric
        // off-diagonal matrix remains integral at denominator 2^bits.
        cpp_int half_precision;
        if (!quantize_dyadic(std::max(0.0, triangle_multipliers[i]),
                             fractional_bits - 1, half_precision))
            return certificate;
        exact.numerator = half_precision * 2;
        certificate.triangle_multipliers.push_back(std::move(exact));
    }

    const auto slack = bisection_slack_numerator(
        graph, certificate.y_numerators, certificate.z_numerator,
        certificate.denominator, certificate.triangle_multipliers);
    if (slack.size() != graph.size()) return certificate;
    certificate.diagonal_repair_numerators.resize(graph.size());
    for (std::size_t i = 0; i < graph.size(); ++i) {
        cpp_int margin = slack[i][i];
        for (std::size_t j = 0; j < graph.size(); ++j)
            if (i != j) margin -= abs_int(slack[i][j]);
        certificate.diagonal_repair_numerators[i] =
            margin < 0 ? -margin : cpp_int(0);
    }
    if (const auto shift = minimum_strict_shift(slack)) {
        certificate.proof_kind = BisectionProofKind::exact_shifted_bareiss;
        std::fill(certificate.diagonal_repair_numerators.begin(),
                  certificate.diagonal_repair_numerators.end(), *shift);
    }

    certificate.objective_numerator = certificate.z_numerator;
    const cpp_int h = cpp_int(2) * cardinality - graph.size();
    certificate.objective_numerator *= h * h;
    for (std::size_t i = 0; i < graph.size(); ++i)
        certificate.objective_numerator += certificate.y_numerators[i]
            - certificate.diagonal_repair_numerators[i];
    for (const auto& triangle : certificate.triangle_multipliers)
        certificate.objective_numerator -= triangle.numerator;
    certificate.integer_lower_bound = nonnegative_ceiling(
        certificate.objective_numerator, certificate.denominator);
    certificate.valid = certificate.integer_lower_bound.has_value() &&
                        verify_bisection_certificate(graph, certificate);
    return certificate;
}

bool verify_bisection_certificate(
    const Graph& graph, const BisectionCertificate& certificate) {
    if (!certificate.integer_lower_bound || graph.size() == 0 ||
        certificate.vertex_count != graph.size() || certificate.cardinality == 0 ||
        certificate.cardinality >= graph.size() || certificate.fractional_bits < 2 ||
        certificate.fractional_bits > 50 ||
        certificate.y_numerators.size() != graph.size() ||
        certificate.diagonal_repair_numerators.size() != graph.size() ||
        certificate.denominator != (cpp_int(1) << certificate.fractional_bits))
        return false;
    for (const auto& repair : certificate.diagonal_repair_numerators)
        if (repair < 0) return false;
    for (const auto& triangle : certificate.triangle_multipliers) {
        if (triangle.numerator < 0 || (triangle.numerator & 1) != 0 ||
            !(triangle.cut.a < triangle.cut.b && triangle.cut.b < triangle.cut.c &&
              triangle.cut.c < graph.size() && triangle.cut.form < 4))
            return false;
    }
    const auto slack = bisection_slack_numerator(
        graph, certificate.y_numerators, certificate.z_numerator,
        certificate.denominator, certificate.triangle_multipliers);
    if (slack.size() != graph.size()) return false;
    if (certificate.proof_kind == BisectionProofKind::rowwise_diagonal_dominance) {
        for (std::size_t i = 0; i < graph.size(); ++i) {
            cpp_int bound = slack[i][i] + certificate.diagonal_repair_numerators[i];
            for (std::size_t j = 0; j < graph.size(); ++j) {
                if (i == j) continue;
                bound -= abs_int(slack[i][j]);
            }
            if (bound < 0) return false;
        }
    } else if (certificate.proof_kind == BisectionProofKind::exact_shifted_bareiss) {
        const cpp_int& shift = certificate.diagonal_repair_numerators.front();
        for (const auto& repair : certificate.diagonal_repair_numerators)
            if (repair != shift) return false;
        const auto positive = strictly_positive_definite(slack, shift);
        if (!positive || !*positive) return false;
    } else {
        return false;
    }
    cpp_int objective = certificate.z_numerator;
    const cpp_int h = cpp_int(2) * certificate.cardinality - graph.size();
    objective *= h * h;
    for (std::size_t i = 0; i < graph.size(); ++i)
        objective += certificate.y_numerators[i]
            - certificate.diagonal_repair_numerators[i];
    for (const auto& triangle : certificate.triangle_multipliers)
        objective -= triangle.numerator;
    if (objective != certificate.objective_numerator) return false;
    const auto lower_bound = nonnegative_ceiling(objective, certificate.denominator);
    return lower_bound && *lower_bound == *certificate.integer_lower_bound;
}

namespace {
constexpr int triangle_signs[4][3] = {
    {1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};

std::vector<BisectionTriangleCut> separate_bisection_triangles(
    std::size_t n, std::span<const double> packed_moment, std::size_t limit) {
    struct Violated { double slack; BisectionTriangleCut cut; };
    std::vector<Violated> violated;
    if (packed_moment.size() != n * (n + 1) / 2 || limit == 0) return {};
    const auto correlation = [&](std::size_t i, std::size_t j) {
        if (i > j) std::swap(i, j);
        const auto packed = j * (j + 1) / 2 + i;
        return i == j ? packed_moment[packed]
                      : packed_moment[packed] / std::sqrt(2.0);
    };
    for (std::uint32_t a = 0; a < n; ++a)
        for (std::uint32_t b = a + 1; b < n; ++b)
            for (std::uint32_t c = b + 1; c < n; ++c) {
                const double values[3] = {
                    correlation(a, b), correlation(a, c), correlation(b, c)};
                for (std::uint8_t form = 0; form < 4; ++form) {
                    double slack = 1.0;
                    for (std::size_t pair = 0; pair < 3; ++pair)
                        slack += triangle_signs[form][pair] * values[pair];
                    if (slack < -1e-8) violated.push_back({slack, {a, b, c, form}});
                }
            }
    std::stable_sort(violated.begin(), violated.end(), [](const auto& x, const auto& y) {
        if (x.slack != y.slack) return x.slack < y.slack;
        if (x.cut.a != y.cut.a) return x.cut.a < y.cut.a;
        if (x.cut.b != y.cut.b) return x.cut.b < y.cut.b;
        if (x.cut.c != y.cut.c) return x.cut.c < y.cut.c;
        return x.cut.form < y.cut.form;
    });
    if (violated.size() > limit) violated.resize(limit);
    std::vector<BisectionTriangleCut> cuts;
    cuts.reserve(violated.size());
    for (const auto& item : violated) cuts.push_back(item.cut);
    return cuts;
}

ClarabelBisectionResult solve_bisection_triangle_model(
    const Graph& graph, const ClarabelBisectionOptions& options,
    std::span<const BisectionTriangleCut> cuts) {
    ClarabelBisectionResult result;
#ifndef CUTWIDTH_HAVE_CLARABEL_SDP
    (void)graph; (void)options; (void)cuts;
    result.diagnostic = "Clarabel.cpp SDP backend was not configured";
    return result;
#else
    const auto wall_started = std::chrono::steady_clock::now();
    const std::size_t n = graph.size();
    const std::size_t psd_rows = n * (n + 1) / 2;
    const std::size_t variables = n + 1 + cuts.size();
    const std::size_t rows = psd_rows + cuts.size();
    using Triplet = Eigen::Triplet<double>;
    std::vector<Triplet> triplets;
    triplets.reserve(psd_rows + n + 4 * cuts.size());
    Eigen::VectorXd b(static_cast<Eigen::Index>(rows));
    b.setZero();
    const double sqrt2 = std::sqrt(2.0);
    std::size_t cone_row = 0;
    for (std::size_t column = 0; column < n; ++column) {
        for (std::size_t row = 0; row <= column; ++row, ++cone_row) {
            if (row == column) {
                triplets.emplace_back(cone_row, row, 1.0);
                triplets.emplace_back(cone_row, n, 1.0);
                b[cone_row] = static_cast<double>(
                    graph.degree(static_cast<Graph::Vertex>(row))) / 4.0;
            } else {
                triplets.emplace_back(cone_row, n, sqrt2);
                b[cone_row] = graph.adjacent(static_cast<Graph::Vertex>(row),
                                              static_cast<Graph::Vertex>(column))
                    ? -sqrt2 / 4.0 : 0.0;
            }
        }
    }
    for (std::size_t index = 0; index < cuts.size(); ++index) {
        const auto& cut = cuts[index];
        const std::size_t ends[3][2] = {
            {cut.a, cut.b}, {cut.a, cut.c}, {cut.b, cut.c}};
        for (std::size_t pair = 0; pair < 3; ++pair) {
            const auto i = ends[pair][0], j = ends[pair][1];
            const auto row = j * (j + 1) / 2 + i;
            triplets.emplace_back(row, n + 1 + index,
                static_cast<double>(triangle_signs[cut.form][pair]) / sqrt2);
        }
        // -lambda + s = 0 with s in the nonnegative cone enforces lambda >= 0.
        triplets.emplace_back(psd_rows + index, n + 1 + index, -1.0);
    }
    Eigen::SparseMatrix<double> P(variables, variables);
    Eigen::VectorXd q(variables);
    q.head(static_cast<Eigen::Index>(n)).setConstant(-1.0);
    const double h = 2.0 * static_cast<double>(options.cardinality) - n;
    q[n] = -(h * h);
    if (!cuts.empty()) q.tail(static_cast<Eigen::Index>(cuts.size())).setConstant(1.0);
    Eigen::SparseMatrix<double> A(rows, variables);
    A.setFromTriplets(triplets.begin(), triplets.end()); A.makeCompressed();
    std::vector<clarabel::SupportedConeT<double>> cones{
        clarabel::PSDTriangleConeT<double>(n),
        clarabel::NonnegativeConeT<double>(cuts.size())};
    auto settings = clarabel::DefaultSettings<double>::default_settings();
    settings.verbose = false;
    settings.max_iter = static_cast<std::uint32_t>(options.max_iterations);
    if (options.time_limit_seconds > 0) {
        const double built = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wall_started).count();
        if (built >= options.time_limit_seconds) {
            result.status = ClarabelSdpStatus::max_time;
            result.diagnostic = "triangle SDP budget exhausted during model construction";
            return result;
        }
        settings.time_limit = std::max(0.001, options.time_limit_seconds - built);
    }
    clarabel::DefaultSolver<double> solver(P, q, A, b, cones, settings);
    DeadlineCallback callback{std::chrono::steady_clock::time_point::max(), false};
    if (options.time_limit_seconds > 0) {
        callback.deadline = wall_started + std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(options.time_limit_seconds));
        solver.set_termination_callback(stop_at_deadline, &callback);
    }
    solver.solve();
    if (options.time_limit_seconds > 0) solver.unset_termination_callback();
    const auto solution = solver.solution();
    using clarabel::SolverStatus;
    switch (solution.status) {
    case SolverStatus::Solved: result.status = ClarabelSdpStatus::solved; break;
    case SolverStatus::AlmostSolved: result.status = ClarabelSdpStatus::almost_solved; break;
    case SolverStatus::MaxIterations: result.status = ClarabelSdpStatus::max_iterations; break;
    case SolverStatus::MaxTime: result.status = ClarabelSdpStatus::max_time; break;
    case SolverStatus::PrimalInfeasible:
    case SolverStatus::AlmostPrimalInfeasible: result.status = ClarabelSdpStatus::infeasible; break;
    case SolverStatus::NumericalError: result.status = ClarabelSdpStatus::numerical_error; break;
    case SolverStatus::CallbackTerminated:
        result.status = callback.expired ? ClarabelSdpStatus::max_time
                                         : ClarabelSdpStatus::unknown;
        break;
    default: result.status = ClarabelSdpStatus::unknown; break;
    }
    result.raw_dual_bound = -solution.obj_val;
    result.primal_residual = solution.r_prim;
    result.dual_residual = solution.r_dual;
    result.solve_seconds = solution.solve_time;
    result.iterations = solution.iterations;
    result.triangle_cuts = cuts.size();
    if (solution.z.size() >= static_cast<Eigen::Index>(psd_rows))
        result.packed_moment.assign(solution.z.begin(), solution.z.begin() + psd_rows);
    if (solution.x.size() == static_cast<Eigen::Index>(variables)) {
        std::vector<double> y(n), multipliers(cuts.size());
        for (std::size_t i = 0; i < n; ++i) y[i] = solution.x[i];
        for (std::size_t i = 0; i < cuts.size(); ++i)
            multipliers[i] = solution.x[n + 1 + i];
        result.certificate = recover_bisection_certificate(
            graph, options.cardinality, y, solution.x[n],
            options.quantization_bits, cuts, multipliers);
        if (!result.certificate.valid)
            result.diagnostic =
                "triangle Clarabel iterate did not yield a checkable dyadic certificate";
    }
    return result;
#endif
}
} // namespace

ClarabelBisectionResult solve_bisection_sdp_clarabel(
    const Graph& graph, const ClarabelBisectionOptions& options) {
    ClarabelBisectionResult result;
#ifndef CUTWIDTH_HAVE_CLARABEL_SDP
    (void)graph;
    (void)options;
    result.diagnostic = "Clarabel.cpp SDP backend was not configured";
    return result;
#else
    const auto wall_started = std::chrono::steady_clock::now();
    const std::size_t n = graph.size();
    if (n == 0 || options.cardinality == 0 || options.cardinality >= n) {
        result.status = ClarabelSdpStatus::unsupported;
        result.diagnostic = "bisection cardinality must lie strictly between zero and n";
        return result;
    }
    if (options.max_iterations > std::numeric_limits<std::uint32_t>::max() ||
        !std::isfinite(options.time_limit_seconds) || options.time_limit_seconds < 0 ||
        options.quantization_bits < 2 || options.quantization_bits > 50)
        throw std::invalid_argument("invalid Clarabel bisection SDP options");
    if (options.triangle_cut_limit != 0) {
        auto compact_options = options;
        compact_options.triangle_cut_limit = 0;
        if (options.time_limit_seconds > 0)
            compact_options.time_limit_seconds = std::min(
                3.0, std::max(0.25, options.time_limit_seconds * 0.4));
        auto compact = solve_bisection_sdp_clarabel(graph, compact_options);
        const auto cuts = separate_bisection_triangles(
            n, compact.packed_moment, options.triangle_cut_limit);
        if (cuts.empty()) return compact;
        auto strengthened_options = options;
        strengthened_options.triangle_cut_limit = 0;
        if (options.time_limit_seconds > 0) {
            const double used = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - wall_started).count();
            if (used >= options.time_limit_seconds) return compact;
            strengthened_options.time_limit_seconds = options.time_limit_seconds - used;
        }
        auto strengthened = solve_bisection_triangle_model(
            graph, strengthened_options, cuts);
        strengthened.solve_seconds += compact.solve_seconds;
        strengthened.iterations += compact.iterations;
        const auto compact_bound = compact.certificate.integer_lower_bound.value_or(0);
        const auto strengthened_bound =
            strengthened.certificate.integer_lower_bound.value_or(0);
        if (!strengthened.certificate.valid || strengthened_bound < compact_bound)
            return compact;
        return strengthened;
    }

    using Triplet = Eigen::Triplet<double>;
    const std::size_t variables = n + 1;
    const std::size_t rows = n * (n + 1) / 2;
    std::vector<Triplet> triplets;
    triplets.reserve(rows + n);
    Eigen::VectorXd b(static_cast<Eigen::Index>(rows));
    const double sqrt2 = std::sqrt(2.0);
    std::size_t cone_row = 0;
    for (std::size_t column = 0; column < n; ++column) {
        for (std::size_t row = 0; row <= column; ++row, ++cone_row) {
            if (row == column) {
                triplets.emplace_back(static_cast<Eigen::Index>(cone_row),
                                      static_cast<Eigen::Index>(row), 1.0);
                triplets.emplace_back(static_cast<Eigen::Index>(cone_row),
                                      static_cast<Eigen::Index>(n), 1.0);
                b[static_cast<Eigen::Index>(cone_row)] =
                    static_cast<double>(graph.degree(static_cast<Graph::Vertex>(row))) / 4.0;
            } else {
                triplets.emplace_back(static_cast<Eigen::Index>(cone_row),
                                      static_cast<Eigen::Index>(n), sqrt2);
                b[static_cast<Eigen::Index>(cone_row)] = graph.adjacent(
                    static_cast<Graph::Vertex>(row), static_cast<Graph::Vertex>(column))
                    ? -sqrt2 / 4.0 : 0.0;
            }
        }
    }
    Eigen::SparseMatrix<double> P(static_cast<Eigen::Index>(variables),
                                  static_cast<Eigen::Index>(variables));
    Eigen::VectorXd q(static_cast<Eigen::Index>(variables));
    q.head(static_cast<Eigen::Index>(n)).setConstant(-1.0);
    const double h = 2.0 * static_cast<double>(options.cardinality) - static_cast<double>(n);
    q[static_cast<Eigen::Index>(n)] = -(h * h);
    Eigen::SparseMatrix<double> A(static_cast<Eigen::Index>(rows),
                                  static_cast<Eigen::Index>(variables));
    A.setFromTriplets(triplets.begin(), triplets.end());
    A.makeCompressed();
    std::vector<clarabel::SupportedConeT<double>> cones{
        clarabel::PSDTriangleConeT<double>(n)};
    auto settings = clarabel::DefaultSettings<double>::default_settings();
    settings.verbose = false;
    settings.max_iter = static_cast<std::uint32_t>(options.max_iterations);
    if (options.time_limit_seconds > 0) {
        const double build_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wall_started).count();
        if (build_seconds >= options.time_limit_seconds) {
            result.status = ClarabelSdpStatus::max_time;
            result.diagnostic = "Clarabel budget exhausted during model construction";
            return result;
        }
        settings.time_limit = std::max(0.001, options.time_limit_seconds - build_seconds);
    }
    clarabel::DefaultSolver<double> solver(P, q, A, b, cones, settings);
    DeadlineCallback callback{std::chrono::steady_clock::time_point::max(), false};
    if (options.time_limit_seconds > 0) {
        callback.deadline = wall_started + std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(options.time_limit_seconds));
        solver.set_termination_callback(stop_at_deadline, &callback);
    }
    solver.solve();
    if (options.time_limit_seconds > 0) solver.unset_termination_callback();
    const auto solution = solver.solution();
    using clarabel::SolverStatus;
    switch (solution.status) {
    case SolverStatus::Solved: result.status = ClarabelSdpStatus::solved; break;
    case SolverStatus::AlmostSolved: result.status = ClarabelSdpStatus::almost_solved; break;
    case SolverStatus::MaxIterations: result.status = ClarabelSdpStatus::max_iterations; break;
    case SolverStatus::MaxTime: result.status = ClarabelSdpStatus::max_time; break;
    case SolverStatus::PrimalInfeasible:
    case SolverStatus::AlmostPrimalInfeasible: result.status = ClarabelSdpStatus::infeasible; break;
    case SolverStatus::NumericalError: result.status = ClarabelSdpStatus::numerical_error; break;
    case SolverStatus::CallbackTerminated:
        result.status = callback.expired ? ClarabelSdpStatus::max_time
                                         : ClarabelSdpStatus::unknown;
        break;
    default: result.status = ClarabelSdpStatus::unknown; break;
    }
    result.raw_dual_bound = -solution.obj_val;
    result.primal_residual = solution.r_prim;
    result.dual_residual = solution.r_dual;
    result.solve_seconds = solution.solve_time;
    result.iterations = solution.iterations;
    if (solution.z.size() >= static_cast<Eigen::Index>(rows))
        result.packed_moment.assign(solution.z.begin(), solution.z.begin() + rows);
    if (solution.x.size() == static_cast<Eigen::Index>(variables)) {
        std::vector<double> y(n);
        for (std::size_t i = 0; i < n; ++i) y[i] = solution.x[static_cast<Eigen::Index>(i)];
        result.certificate = recover_bisection_certificate(
            graph, options.cardinality, y, solution.x[static_cast<Eigen::Index>(n)],
            options.quantization_bits);
        if (!result.certificate.valid)
            result.diagnostic = "Clarabel iterate did not yield a checkable dyadic certificate";
    }
    return result;
#endif
}

namespace {
std::uint64_t weighted_model_hash(const Graph& graph,
                                  std::span<const std::uint32_t> boundary) {
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (8 * byte)) & 0xffU;
            hash *= 1099511628211ULL;
        }
    };
    mix(graph.size());
    for (std::size_t i = 0; i < graph.size(); ++i) {
        mix(boundary[i]);
        for (std::size_t j = i + 1; j < graph.size(); ++j)
            mix(graph.adjacent(static_cast<Graph::Vertex>(i),
                               static_cast<Graph::Vertex>(j)) ? 1 : 0);
    }
    return hash;
}
IntegerMatrix weighted_bisection_slack_numerator(
    const Graph& graph, std::span<const std::uint32_t> boundary,
    const cpp_int& a, const std::vector<cpp_int>& y, const cpp_int& r,
    const cpp_int& z, const cpp_int& denominator) {
    const std::size_t n = graph.size();
    IntegerMatrix matrix(n + 1, std::vector<cpp_int>(n + 1));
    const cpp_int quarter = denominator / 4;
    cpp_int boundary_sum = 0;
    for (const auto value : boundary) boundary_sum += value;
    matrix[0][0] = denominator * boundary_sum / 2 - a;
    for (std::size_t i = 0; i < n; ++i) {
        matrix[0][i + 1] = -quarter * boundary[i] - r / 2;
        matrix[i + 1][0] = matrix[0][i + 1];
        matrix[i + 1][i + 1] =
            quarter * graph.degree(static_cast<Graph::Vertex>(i)) - y[i] - z;
        for (std::size_t j = i + 1; j < n; ++j) {
            cpp_int entry = -z;
            if (graph.adjacent(static_cast<Graph::Vertex>(i),
                               static_cast<Graph::Vertex>(j)))
                entry -= quarter;
            matrix[i + 1][j + 1] = entry;
            matrix[j + 1][i + 1] = entry;
        }
    }
    return matrix;
}
} // namespace

WeightedBisectionCertificate recover_weighted_bisection_certificate(
    const Graph& graph, std::span<const std::uint32_t> boundary,
    std::size_t cardinality, double a, std::span<const double> y,
    double r, double z, unsigned fractional_bits) {
    WeightedBisectionCertificate certificate;
    certificate.vertex_count = graph.size();
    if (boundary.size() == graph.size())
        certificate.model_hash = weighted_model_hash(graph, boundary);
    certificate.cardinality = cardinality;
    certificate.fractional_bits = fractional_bits;
    if (graph.size() == 0 || cardinality == 0 || cardinality >= graph.size() ||
        boundary.size() != graph.size() || y.size() != graph.size() ||
        fractional_bits < 2 || fractional_bits > 50)
        return certificate;
    certificate.denominator = cpp_int(1) << fractional_bits;
    cpp_int half_precision_r;
    if (!quantize_dyadic(a, fractional_bits, certificate.constant_numerator) ||
        // The symmetric 0-i entry contains r/2. Quantize r on the next
        // coarser dyadic grid so its numerator is even and the entire exact
        // slack continues to use the advertised common denominator.
        !quantize_dyadic(r, fractional_bits - 1, half_precision_r) ||
        !quantize_dyadic(z, fractional_bits,
                         certificate.squared_cardinality_numerator))
        return certificate;
    certificate.linear_cardinality_numerator = half_precision_r * 2;
    certificate.y_numerators.resize(graph.size());
    for (std::size_t i = 0; i < graph.size(); ++i)
        if (!quantize_dyadic(y[i], fractional_bits, certificate.y_numerators[i]))
            return certificate;

    const auto slack = weighted_bisection_slack_numerator(
        graph, boundary, certificate.constant_numerator, certificate.y_numerators,
        certificate.linear_cardinality_numerator,
        certificate.squared_cardinality_numerator, certificate.denominator);
    certificate.diagonal_repair_numerators.resize(graph.size() + 1);
    for (std::size_t i = 0; i < slack.size(); ++i) {
        cpp_int margin = slack[i][i];
        for (std::size_t j = 0; j < slack.size(); ++j)
            if (i != j) margin -= abs_int(slack[i][j]);
        certificate.diagonal_repair_numerators[i] =
            margin < 0 ? -margin : cpp_int(0);
    }
    if (const auto shift = minimum_strict_shift(slack)) {
        certificate.proof_kind = BisectionProofKind::exact_shifted_bareiss;
        std::fill(certificate.diagonal_repair_numerators.begin(),
                  certificate.diagonal_repair_numerators.end(), *shift);
    }
    const cpp_int h = cpp_int(2) * cardinality - graph.size();
    certificate.objective_numerator = certificate.constant_numerator
        + certificate.linear_cardinality_numerator * h
        + certificate.squared_cardinality_numerator * h * h;
    for (const auto& value : certificate.y_numerators)
        certificate.objective_numerator += value;
    for (const auto& repair : certificate.diagonal_repair_numerators)
        certificate.objective_numerator -= repair;
    certificate.integer_lower_bound = nonnegative_ceiling(
        certificate.objective_numerator, certificate.denominator);
    certificate.valid = certificate.integer_lower_bound.has_value() &&
        verify_weighted_bisection_certificate(graph, boundary, certificate);
    return certificate;
}

bool verify_weighted_bisection_certificate(
    const Graph& graph, std::span<const std::uint32_t> boundary,
    const WeightedBisectionCertificate& certificate) {
    if (!certificate.integer_lower_bound || graph.size() == 0 ||
        certificate.vertex_count != graph.size() || boundary.size() != graph.size() ||
        certificate.model_hash != weighted_model_hash(graph, boundary) ||
        certificate.cardinality == 0 || certificate.cardinality >= graph.size() ||
        certificate.fractional_bits < 2 || certificate.fractional_bits > 50 ||
        certificate.y_numerators.size() != graph.size() ||
        certificate.diagonal_repair_numerators.size() != graph.size() + 1 ||
        certificate.denominator != (cpp_int(1) << certificate.fractional_bits))
        return false;
    for (const auto& repair : certificate.diagonal_repair_numerators)
        if (repair < 0) return false;
    const auto slack = weighted_bisection_slack_numerator(
        graph, boundary, certificate.constant_numerator, certificate.y_numerators,
        certificate.linear_cardinality_numerator,
        certificate.squared_cardinality_numerator, certificate.denominator);
    if (certificate.proof_kind == BisectionProofKind::rowwise_diagonal_dominance) {
        for (std::size_t i = 0; i < slack.size(); ++i) {
            cpp_int margin = slack[i][i] + certificate.diagonal_repair_numerators[i];
            for (std::size_t j = 0; j < slack.size(); ++j)
                if (i != j) margin -= abs_int(slack[i][j]);
            if (margin < 0) return false;
        }
    } else if (certificate.proof_kind == BisectionProofKind::exact_shifted_bareiss) {
        const auto& shift = certificate.diagonal_repair_numerators.front();
        for (const auto& repair : certificate.diagonal_repair_numerators)
            if (repair != shift) return false;
        const auto positive = strictly_positive_definite(slack, shift);
        if (!positive || !*positive) return false;
    } else return false;
    const cpp_int h = cpp_int(2) * certificate.cardinality - graph.size();
    cpp_int objective = certificate.constant_numerator
        + certificate.linear_cardinality_numerator * h
        + certificate.squared_cardinality_numerator * h * h;
    for (const auto& value : certificate.y_numerators) objective += value;
    for (const auto& repair : certificate.diagonal_repair_numerators) objective -= repair;
    if (objective != certificate.objective_numerator) return false;
    const auto lower = nonnegative_ceiling(objective, certificate.denominator);
    return lower && *lower == *certificate.integer_lower_bound;
}

ClarabelWeightedBisectionResult solve_weighted_bisection_sdp_clarabel(
    const Graph& graph, std::span<const std::uint32_t> boundary,
    const ClarabelWeightedBisectionOptions& options) {
    ClarabelWeightedBisectionResult result;
#ifndef CUTWIDTH_HAVE_CLARABEL_SDP
    (void)graph; (void)boundary; (void)options;
    result.diagnostic = "Clarabel.cpp SDP backend was not configured";
    return result;
#else
    const auto wall_started = std::chrono::steady_clock::now();
    const std::size_t n = graph.size(), d = n + 1;
    if (n == 0 || boundary.size() != n || options.cardinality == 0 ||
        options.cardinality >= n) {
        result.status = ClarabelSdpStatus::unsupported;
        result.diagnostic = "weighted bisection dimensions/cardinality are invalid";
        return result;
    }
    if (options.max_iterations > std::numeric_limits<std::uint32_t>::max() ||
        !std::isfinite(options.time_limit_seconds) || options.time_limit_seconds < 0 ||
        options.quantization_bits < 2 || options.quantization_bits > 50)
        throw std::invalid_argument("invalid Clarabel weighted bisection SDP options");
    const std::size_t variables = n + 3;
    const std::size_t rows = d * (d + 1) / 2;
    using Triplet = Eigen::Triplet<double>;
    std::vector<Triplet> triplets;
    Eigen::VectorXd b(static_cast<Eigen::Index>(rows));
    const double sqrt2 = std::sqrt(2.0);
    double boundary_sum = 0.0;
    for (const auto value : boundary) boundary_sum += value;
    std::size_t cone_row = 0;
    for (std::size_t column = 0; column < d; ++column) {
        for (std::size_t row = 0; row <= column; ++row, ++cone_row) {
            if (row == 0 && column == 0) {
                triplets.emplace_back(cone_row, 0, 1.0);
                b[cone_row] = boundary_sum / 2.0;
            } else if (row == 0) {
                const std::size_t i = column - 1;
                triplets.emplace_back(cone_row, n + 1, 1.0 / sqrt2);
                b[cone_row] = -static_cast<double>(boundary[i]) / (2.0 * sqrt2);
            } else if (row == column) {
                const std::size_t i = row - 1;
                triplets.emplace_back(cone_row, 1 + i, 1.0);
                triplets.emplace_back(cone_row, n + 2, 1.0);
                b[cone_row] = static_cast<double>(
                    graph.degree(static_cast<Graph::Vertex>(i))) / 4.0;
            } else {
                const std::size_t i = row - 1, j = column - 1;
                triplets.emplace_back(cone_row, n + 2, sqrt2);
                b[cone_row] = graph.adjacent(static_cast<Graph::Vertex>(i),
                                             static_cast<Graph::Vertex>(j))
                    ? -sqrt2 / 4.0 : 0.0;
            }
        }
    }
    Eigen::SparseMatrix<double> P(variables, variables);
    Eigen::VectorXd q(variables);
    q[0] = -1.0;
    q.segment(1, static_cast<Eigen::Index>(n)).setConstant(-1.0);
    const double h = 2.0 * options.cardinality - n;
    q[n + 1] = -h;
    q[n + 2] = -(h * h);
    Eigen::SparseMatrix<double> A(rows, variables);
    A.setFromTriplets(triplets.begin(), triplets.end()); A.makeCompressed();
    std::vector<clarabel::SupportedConeT<double>> cones{clarabel::PSDTriangleConeT<double>(d)};
    auto settings = clarabel::DefaultSettings<double>::default_settings();
    settings.verbose = false;
    settings.max_iter = static_cast<std::uint32_t>(options.max_iterations);
    if (options.time_limit_seconds > 0) {
        const double built = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wall_started).count();
        if (built >= options.time_limit_seconds) {
            result.status = ClarabelSdpStatus::max_time;
            result.diagnostic = "Clarabel budget exhausted during model construction";
            return result;
        }
        settings.time_limit = std::max(0.001, options.time_limit_seconds - built);
    }
    clarabel::DefaultSolver<double> solver(P, q, A, b, cones, settings);
    DeadlineCallback callback{std::chrono::steady_clock::time_point::max(), false};
    if (options.time_limit_seconds > 0) {
        callback.deadline = wall_started + std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(options.time_limit_seconds));
        solver.set_termination_callback(stop_at_deadline, &callback);
    }
    solver.solve();
    if (options.time_limit_seconds > 0) solver.unset_termination_callback();
    const auto solution = solver.solution();
    using clarabel::SolverStatus;
    switch (solution.status) {
    case SolverStatus::Solved: result.status = ClarabelSdpStatus::solved; break;
    case SolverStatus::AlmostSolved: result.status = ClarabelSdpStatus::almost_solved; break;
    case SolverStatus::MaxIterations: result.status = ClarabelSdpStatus::max_iterations; break;
    case SolverStatus::MaxTime: result.status = ClarabelSdpStatus::max_time; break;
    case SolverStatus::PrimalInfeasible:
    case SolverStatus::AlmostPrimalInfeasible: result.status = ClarabelSdpStatus::infeasible; break;
    case SolverStatus::NumericalError: result.status = ClarabelSdpStatus::numerical_error; break;
    case SolverStatus::CallbackTerminated:
        result.status = callback.expired ? ClarabelSdpStatus::max_time : ClarabelSdpStatus::unknown;
        break;
    default: result.status = ClarabelSdpStatus::unknown; break;
    }
    result.raw_dual_bound = -solution.obj_val;
    result.primal_residual = solution.r_prim; result.dual_residual = solution.r_dual;
    result.solve_seconds = solution.solve_time; result.iterations = solution.iterations;
    if (solution.x.size() == static_cast<Eigen::Index>(variables)) {
        std::vector<double> y(n);
        for (std::size_t i = 0; i < n; ++i) y[i] = solution.x[1 + i];
        result.certificate = recover_weighted_bisection_certificate(
            graph, boundary, options.cardinality, solution.x[0], y,
            solution.x[n + 1], solution.x[n + 2], options.quantization_bits);
        if (!result.certificate.valid)
            result.diagnostic = "Clarabel iterate did not yield a checkable weighted dyadic certificate";
    }
    return result;
#endif
}

} // namespace cutwidth::sdp
