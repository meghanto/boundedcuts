#include "sdp_certificate.hpp"

#include <algorithm>
#include <cmath>
#include <cfenv>
#include <limits>
#include <numeric>

namespace cutwidth::sdp {
namespace {

struct Interval { long double low = 0.0L, high = 0.0L; };

long double down(long double value) { return std::nextafterl(value, -INFINITY); }
long double up(long double value) { return std::nextafterl(value, INFINITY); }

Interval exact(double value) {
    const long double promoted = static_cast<long double>(value);
    return {promoted, promoted};
}
Interval add(Interval a, Interval b) { return {down(a.low + b.low), up(a.high + b.high)}; }
Interval multiply(Interval a, Interval b) {
    const long double values[] = {a.low*b.low, a.low*b.high, a.high*b.low, a.high*b.high};
    return {down(*std::min_element(std::begin(values), std::end(values))),
            up(*std::max_element(std::begin(values), std::end(values)))};
}
Interval divide_positive(Interval numerator, Interval denominator) {
    if (denominator.low <= 0) return {-INFINITY, INFINITY};
    return {down(numerator.low / denominator.high), up(numerator.high / denominator.low)};
}
bool finite(Interval value) { return std::isfinite(value.low) && std::isfinite(value.high); }

} // namespace

SdpCertificate recover_basic_certificate(
    const CutwidthSdpOperator& op, const BasicDualCandidate& candidate,
    CertificateOptions options) {
    SdpCertificate result;
    if (!std::numeric_limits<double>::is_iec559 ||
        !std::numeric_limits<long double>::is_iec559 ||
        std::fegetround() != FE_TONEAREST) return result;
    const std::size_t d = op.dimension();
    const std::size_t pairs = d - 1;
    const std::size_t vertices = op.constraint_count() - 1 - pairs;
    if (d > options.max_dimension ||
        candidate.diagonal_multipliers.size() != pairs ||
        candidate.cut_weights.size() != vertices ||
        !std::isfinite(candidate.normalization_multiplier)) return result;

    Interval weight_sum{};
    std::vector<Interval> positive_weights(vertices);
    for (std::size_t v = 0; v < vertices; ++v) {
        const double weight = candidate.cut_weights[v];
        if (!std::isfinite(weight)) return result;
        positive_weights[v] = exact(std::max(0.0, weight));
        weight_sum = add(weight_sum, positive_weights[v]);
    }
    if (!(weight_sum.low > 0) || !finite(weight_sum)) return result;
    std::vector<Interval> mu(vertices);
    for (std::size_t v = 0; v < vertices; ++v) {
        mu[v] = divide_positive(positive_weights[v], weight_sum);
        if (!finite(mu[v])) return result;
    }
    for (const double value : candidate.diagonal_multipliers)
        if (!std::isfinite(value)) return result;

    const std::size_t packed = op.packed_moment_size();
    std::vector<Interval> slack(packed);
    std::vector<double> basis(packed), multiplier(op.constraint_count(), 0.0);
    auto accumulate_basis = [&](std::size_t constraint, Interval scale) {
        std::fill(multiplier.begin(), multiplier.end(), 0.0);
        multiplier[constraint] = 1.0;
        op.apply_adjoint(multiplier, basis);
        for (std::size_t entry = 0; entry < packed; ++entry) {
            if (basis[entry] == 0.0) continue;
            slack[entry] = add(slack[entry], multiply(scale, exact(basis[entry])));
        }
    };
    accumulate_basis(0, exact(candidate.normalization_multiplier));
    for (std::size_t pair = 0; pair < pairs; ++pair)
        accumulate_basis(1 + pair, exact(candidate.diagonal_multipliers[pair]));
    for (std::size_t vertex = 0; vertex < vertices; ++vertex)
        accumulate_basis(1 + pairs + vertex, mu[vertex]);
    for (const auto value : slack) if (!finite(value)) return result;

    long double lambda_lower = INFINITY;
    for (std::size_t row = 0; row < d; ++row) {
        const auto diagonal = slack[packed_index(d, row, row)];
        long double radius_upper = 0.0L;
        for (std::size_t column = 0; column < d; ++column) {
            if (column == row) continue;
            const auto coefficient = slack[packed_index(d, row, column)];
            // A polynomial off-diagonal coefficient q represents symmetric
            // matrix entries q/2 in the Frobenius dual slack.
            const long double magnitude = up(std::max(std::abs(coefficient.low),
                                                       std::abs(coefficient.high)) / 2.0L);
            radius_upper = up(radius_upper + magnitude);
        }
        lambda_lower = std::min(lambda_lower, down(diagonal.low - radius_upper));
    }
    if (!std::isfinite(lambda_lower)) return result;
    const long double rho = lambda_lower < 0 ? up(-lambda_lower) : 0.0L;
    const long double objective_lower = down(-static_cast<long double>(
        candidate.normalization_multiplier));
    long double corrected = down(objective_lower - up(rho * static_cast<long double>(d)));
    // Cutwidth is independently known nonnegative, so this strengthens a very
    // conservative corrected dual without relying on the approximate solver.
    corrected = std::max(0.0L, corrected);
    if (!std::isfinite(corrected) || corrected > std::numeric_limits<std::uint32_t>::max())
        return result;
    const long double integer = std::ceil(corrected);
    result.valid = true;
    result.corrected_dual_bound = static_cast<double>(down(corrected));
    result.gershgorin_lower_eigenvalue = static_cast<double>(down(lambda_lower));
    result.trace_penalty = static_cast<double>(up(rho * static_cast<long double>(d)));
    result.integer_lower_bound = static_cast<std::uint32_t>(integer);
    return result;
}

} // namespace cutwidth::sdp
