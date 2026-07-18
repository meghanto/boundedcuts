#pragma once

#include "sdp_operator.hpp"

#include <cstddef>
#include <vector>

namespace cutwidth::sdp {

struct DenseAdmmOptions {
    std::size_t iterations = 500;
    std::size_t constraint_projection_sweeps = 3;
    std::size_t max_dimension = 256;
    double alpha = 0.0;
    double tolerance = 1e-6;
};

struct DenseAdmmResult {
    bool converged = false;
    std::size_t iterations = 0;
    double primal_residual = 0.0;
    double equality_residual = 0.0;
    double cut_violation = 0.0;
    std::vector<double> packed_moment;
};

// Diagnostic fixed-alpha feasibility prototype. It never returns a certified
// lower bound and is not consumed by exact-search pruning.
[[nodiscard]] DenseAdmmResult solve_dense_feasibility_admm(
    const CutwidthSdpOperator& op, const DenseAdmmOptions& options = {});

} // namespace cutwidth::sdp
