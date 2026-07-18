#pragma once

#include "sdp_certificate.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <boost/multiprecision/cpp_int.hpp>

namespace cutwidth::sdp {

enum class ClarabelSdpStatus {
    solved, almost_solved, max_iterations, max_time, infeasible,
    numerical_error, unavailable, unsupported, unknown
};

struct ClarabelSdpOptions {
    std::size_t max_dimension = 512;
    std::size_t max_iterations = 200;
    std::size_t max_psd_triangle_entries = 12000;
    double time_limit_seconds = 0.0;
    bool chordal_decomposition = true;
};

struct ClarabelSdpResult {
    ClarabelSdpStatus status = ClarabelSdpStatus::unavailable;
    double primal_objective = 0.0;
    double dual_objective = 0.0;
    double primal_residual = 0.0;
    double dual_residual = 0.0;
    double solve_seconds = 0.0;
    std::size_t iterations = 0;
    std::vector<double> packed_moment;
    BasicDualCandidate dual_candidate;
    SdpCertificate certificate;
    std::string diagnostic;
};

[[nodiscard]] ClarabelSdpResult solve_basic_sdp_clarabel(
    const CutwidthSdpOperator& op, const ClarabelSdpOptions& options = {});

enum class BisectionProofKind {
    rowwise_diagonal_dominance,
    exact_shifted_bareiss
};

// Four valid correlation-triangle inequalities are represented by the sign
// patterns +++, +--, -+-, and --+ on (ab, ac, bc):
// 1 + s_ab X_ab + s_ac X_ac + s_bc X_bc >= 0.
struct BisectionTriangleCut {
    std::uint32_t a = 0, b = 0, c = 0;
    std::uint8_t form = 0;
};

struct BisectionTriangleMultiplier {
    BisectionTriangleCut cut;
    boost::multiprecision::cpp_int numerator = 0;
};

struct BisectionCertificate {
    bool valid = false;
    std::size_t vertex_count = 0;
    std::size_t cardinality = 0;
    unsigned fractional_bits = 0;
    BisectionProofKind proof_kind = BisectionProofKind::rowwise_diagonal_dominance;
    std::vector<boost::multiprecision::cpp_int> y_numerators;
    boost::multiprecision::cpp_int z_numerator = 0;
    // Per-row nonnegative diagonal additions to the dual slack.  Equivalently,
    // the certified dual variables are y_i - diagonal_repair_numerators[i]/Q.
    std::vector<boost::multiprecision::cpp_int> diagonal_repair_numerators;
    std::vector<BisectionTriangleMultiplier> triangle_multipliers;
    boost::multiprecision::cpp_int objective_numerator = 0;
    boost::multiprecision::cpp_int denominator = 1;
    std::optional<std::uint32_t> integer_lower_bound;
};

struct ClarabelBisectionOptions {
    std::size_t cardinality = 0;
    std::size_t max_iterations = 200;
    double time_limit_seconds = 0.0;
    unsigned quantization_bits = 30;
    // One separation round adds at most this many most-violated valid
    // correlation-triangle inequalities. Zero preserves the compact model.
    std::size_t triangle_cut_limit = 0;
};

struct ClarabelBisectionResult {
    ClarabelSdpStatus status = ClarabelSdpStatus::unavailable;
    double raw_dual_bound = 0.0;
    double primal_residual = 0.0;
    double dual_residual = 0.0;
    double solve_seconds = 0.0;
    std::size_t iterations = 0;
    std::size_t triangle_cuts = 0;
    std::vector<double> packed_moment;
    BisectionCertificate certificate;
    std::string diagnostic;
};

[[nodiscard]] ClarabelBisectionResult solve_bisection_sdp_clarabel(
    const Graph& graph, const ClarabelBisectionOptions& options);
[[nodiscard]] BisectionCertificate recover_bisection_certificate(
    const Graph& graph, std::size_t cardinality,
    std::span<const double> y, double z, unsigned fractional_bits = 30,
    std::span<const BisectionTriangleCut> triangle_cuts = {},
    std::span<const double> triangle_multipliers = {});
[[nodiscard]] bool verify_bisection_certificate(
    const Graph& graph, const BisectionCertificate& certificate);

struct WeightedBisectionCertificate {
    bool valid = false;
    std::uint64_t model_hash = 0;
    std::size_t vertex_count = 0;
    std::size_t cardinality = 0;
    unsigned fractional_bits = 0;
    BisectionProofKind proof_kind = BisectionProofKind::rowwise_diagonal_dominance;
    boost::multiprecision::cpp_int constant_numerator = 0;
    std::vector<boost::multiprecision::cpp_int> y_numerators;
    boost::multiprecision::cpp_int linear_cardinality_numerator = 0;
    boost::multiprecision::cpp_int squared_cardinality_numerator = 0;
    // The first entry repairs the homogenizing diagonal; the remainder repair
    // the residual-vertex diagonals.
    std::vector<boost::multiprecision::cpp_int> diagonal_repair_numerators;
    boost::multiprecision::cpp_int objective_numerator = 0;
    boost::multiprecision::cpp_int denominator = 1;
    std::optional<std::uint32_t> integer_lower_bound;
};

using ClarabelWeightedBisectionOptions = ClarabelBisectionOptions;

struct ClarabelWeightedBisectionResult {
    ClarabelSdpStatus status = ClarabelSdpStatus::unavailable;
    double raw_dual_bound = 0.0;
    double primal_residual = 0.0;
    double dual_residual = 0.0;
    double solve_seconds = 0.0;
    std::size_t iterations = 0;
    WeightedBisectionCertificate certificate;
    std::string diagnostic;
};

[[nodiscard]] ClarabelWeightedBisectionResult solve_weighted_bisection_sdp_clarabel(
    const Graph& residual_graph, std::span<const std::uint32_t> boundary_weights,
    const ClarabelWeightedBisectionOptions& options);
[[nodiscard]] WeightedBisectionCertificate recover_weighted_bisection_certificate(
    const Graph& residual_graph, std::span<const std::uint32_t> boundary_weights,
    std::size_t cardinality, double constant_multiplier,
    std::span<const double> diagonal_multipliers,
    double linear_cardinality_multiplier, double squared_cardinality_multiplier,
    unsigned fractional_bits = 30);
[[nodiscard]] bool verify_weighted_bisection_certificate(
    const Graph& residual_graph, std::span<const std::uint32_t> boundary_weights,
    const WeightedBisectionCertificate& certificate);

} // namespace cutwidth::sdp
