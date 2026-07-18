#pragma once

#include "sdp_operator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cutwidth::sdp {

struct BasicDualCandidate {
    double normalization_multiplier = 0.0;
    std::vector<double> diagonal_multipliers;
    // Arbitrary nonnegative weights; recovery interprets them normalized by
    // their exact real sum, enforcing sum(mu)=1.
    std::vector<double> cut_weights;
};

struct CertificateOptions {
    std::size_t max_dimension = 256;
};

struct SdpCertificate {
    bool valid = false;
    double corrected_dual_bound = 0.0;
    double gershgorin_lower_eigenvalue = 0.0;
    double trace_penalty = 0.0;
    std::optional<std::uint32_t> integer_lower_bound;
};


[[nodiscard]] SdpCertificate recover_basic_certificate(
    const CutwidthSdpOperator& op, const BasicDualCandidate& candidate,
    CertificateOptions options = {});


} // namespace cutwidth::sdp
