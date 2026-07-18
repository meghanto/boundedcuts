#include "sdp_admm.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

extern "C" void dsyev_(const char* jobz, const char* uplo, const int* n,
                        double* a, const int* lda, double* w,
                        double* work, const int* lwork, int* info);

namespace cutwidth::sdp {
namespace {

std::vector<double> unpack(std::span<const double> packed, std::size_t d) {
    std::vector<double> matrix(d * d);
    for (std::size_t i = 0; i < d; ++i)
        for (std::size_t j = i; j < d; ++j)
            matrix[i + j * d] = matrix[j + i * d] = packed[packed_index(d, i, j)];
    return matrix;
}

std::vector<double> unpack_functional(std::span<const double> packed, std::size_t d) {
    auto matrix = unpack(packed, d);
    for (std::size_t i = 0; i < d; ++i)
        for (std::size_t j = i + 1; j < d; ++j)
            matrix[i + j * d] = matrix[j + i * d] *= 0.5;
    return matrix;
}

std::vector<double> pack(std::span<const double> matrix, std::size_t d) {
    std::vector<double> packed(packed_size(d));
    for (std::size_t i = 0; i < d; ++i)
        for (std::size_t j = i; j < d; ++j)
            packed[packed_index(d, i, j)] = matrix[i + j * d];
    return packed;
}

void project_psd(std::vector<double>& matrix, std::size_t d) {
    if (d > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("PSD projection dimension exceeds LAPACK integer range");
    const int n = static_cast<int>(d), lda = n;
    std::vector<double> eigenvalues(d);
    int lwork = -1, info = 0;
    double query = 0.0;
    const char vectors = 'V', lower = 'L';
    dsyev_(&vectors, &lower, &n, matrix.data(), &lda, eigenvalues.data(),
           &query, &lwork, &info);
    if (info != 0) throw std::runtime_error("LAPACK workspace query failed");
    lwork = std::max(1, static_cast<int>(query));
    std::vector<double> work(static_cast<std::size_t>(lwork));
    dsyev_(&vectors, &lower, &n, matrix.data(), &lda, eigenvalues.data(),
           work.data(), &lwork, &info);
    if (info != 0) throw std::runtime_error("LAPACK PSD eigendecomposition failed");
    const auto eigenvectors = matrix;
    std::fill(matrix.begin(), matrix.end(), 0.0);
    for (std::size_t k = 0; k < d; ++k) {
        const double lambda = std::max(0.0, eigenvalues[k]);
        if (lambda == 0.0) continue;
        for (std::size_t j = 0; j < d; ++j)
            for (std::size_t i = 0; i < d; ++i)
                matrix[i + j * d] += lambda * eigenvectors[i + k * d] *
                                              eigenvectors[j + k * d];
    }
}

double frobenius_distance(std::span<const double> a, std::span<const double> b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double delta = a[i] - b[i];
        sum += delta * delta;
    }
    return std::sqrt(sum);
}

} // namespace

DenseAdmmResult solve_dense_feasibility_admm(
    const CutwidthSdpOperator& op, const DenseAdmmOptions& options) {
    const std::size_t d = op.dimension();
    if (d > options.max_dimension)
        throw std::invalid_argument("SDP exceeds dense ADMM dimension policy");
    if (!std::isfinite(options.alpha) || options.constraint_projection_sweeps == 0)
        throw std::invalid_argument("invalid dense ADMM options");

    std::vector<double> consensus(d * d, 0.0), dual(d * d, 0.0), psd(d * d, 0.0);
    consensus[0] = 1.0;
    const std::size_t constraint_count = op.constraint_count();
    const std::size_t pair_count = d - 1;
    const std::size_t vertex_count = constraint_count - 1 - pair_count;
    std::vector<std::vector<double>> cut_normals(vertex_count);
    std::vector<double> multiplier(constraint_count, 0.0);
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        multiplier[1 + pair_count + vertex] = 1.0;
        std::vector<double> packed(op.packed_moment_size());
        op.apply_adjoint(multiplier, packed);
        cut_normals[vertex] = unpack_functional(packed, d);
        multiplier[1 + pair_count + vertex] = 0.0;
    }

    DenseAdmmResult result;
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        for (std::size_t i = 0; i < psd.size(); ++i) psd[i] = consensus[i] - dual[i];
        project_psd(psd, d);
        for (std::size_t i = 0; i < consensus.size(); ++i) consensus[i] = psd[i] + dual[i];

        for (std::size_t sweep = 0; sweep < options.constraint_projection_sweeps; ++sweep) {
            consensus[0] = 1.0;
            for (std::size_t coordinate = 1; coordinate < d; ++coordinate) {
                const double residual = consensus[coordinate + coordinate * d] -
                                        consensus[coordinate * d];
                const double correction = residual / 1.5;
                consensus[coordinate + coordinate * d] -= correction;
                consensus[coordinate * d] += 0.5 * correction;
                consensus[coordinate] = consensus[coordinate * d];
            }
            for (const auto& normal : cut_normals) {
                double value = 0.0, norm = 0.0;
                for (std::size_t i = 0; i < normal.size(); ++i) {
                    value += normal[i] * consensus[i];
                    norm += normal[i] * normal[i];
                }
                if (value <= options.alpha || norm == 0.0) continue;
                const double scale = (value - options.alpha) / norm;
                for (std::size_t i = 0; i < normal.size(); ++i)
                    consensus[i] -= scale * normal[i];
            }
        }
        for (std::size_t i = 0; i < dual.size(); ++i) dual[i] += psd[i] - consensus[i];
        result.primal_residual = frobenius_distance(psd, consensus);
        result.iterations = iteration + 1;
        if (result.primal_residual <= options.tolerance) {
            result.converged = true;
            break;
        }
    }
    result.packed_moment = pack(consensus, d);
    std::vector<double> constraints(constraint_count);
    op.apply(result.packed_moment, constraints);
    result.equality_residual = std::abs(constraints[0] - 1.0);
    for (std::size_t pair = 0; pair < pair_count; ++pair)
        result.equality_residual = std::max(result.equality_residual,
                                             std::abs(constraints[1 + pair]));
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex)
        result.cut_violation = std::max(result.cut_violation,
            constraints[1 + pair_count + vertex] - options.alpha);
    result.cut_violation = std::max(0.0, result.cut_violation);
    return result;
}

} // namespace cutwidth::sdp
