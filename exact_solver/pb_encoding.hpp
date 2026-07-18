#pragma once

#include "graph.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cutwidth::pb {

// Both encodings are pure CNF and require no pseudo-Boolean solver support.
// sequential_counter is the compact Sinz-style unary counter. totalizer is a
// balanced unary tree (a cardinality-network equivalent interface).
enum class CardinalityEncoding { sequential_counter, totalizer };

struct CnfFormula {
    std::uint32_t variable_count = 0;
    std::vector<std::vector<std::int32_t>> clauses;
};

struct EncodingMetadata {
    std::string model = "cutwidth-prefix-cnf-v1";
    std::string cardinality_encoding;
    std::uint32_t threshold = 0;
    std::uint32_t variables = 0;
    std::uint64_t clauses = 0;
    bool reversal_symmetry_breaking = true;
    bool position_channeling = false;
    // FNV-1a is only a deterministic model fingerprint, not a cryptographic
    // digest. The algorithm name is explicit to prevent stronger claims.
    std::string dimacs_fnv1a64;
};

struct EncodingOptions {
    CardinalityEncoding cardinality = CardinalityEncoding::sequential_counter;
    // Reversal preserves cutwidth, so requiring vertex 0 before vertex 1 is
    // sound for n >= 2. Disable this only for equivalence/debugging tests.
    bool break_reversal_symmetry = true;
    // Replace repeated exact-prefix cardinalities with a permutation matrix
    // channelled through prefix membership recurrences.
    bool channel_positions = false;
};

struct CutwidthCnf {
    CnfFormula formula;
    EncodingMetadata metadata;
    std::size_t vertex_count = 0;
    // prefix_variables[v][p-1] means vertex v lies among the first p vertices,
    // for 1 <= p < n. These identifiers are exposed for witness decoding.
    std::vector<std::vector<std::uint32_t>> prefix_variables;
    // Present only for the channelled formulation. position_variables[v][p]
    // says that vertex v occupies position p.
    std::vector<std::vector<std::uint32_t>> position_variables;
    // Unary crossing counters for each proper prefix. Entry j means at least
    // j+1 edges cross that prefix. Outputs through the encoded threshold are
    // retained so a native incremental solver can add stricter units later.
    std::vector<std::vector<std::uint32_t>> crossing_count_outputs;
};

// CNF for one fixed-prefix cut-profile question:
//   exists T subseteq V \\ S, |T| = cardinality,
//   with |delta(S union T)| <= threshold?
// UNSAT is therefore a certificate that q_t(S) > threshold.  This is much
// smaller than the full ordering encoding and is intended for selectively
// claimed proof-forest fragments.
struct CutProfileCnf {
    CnfFormula formula;
    EncodingMetadata metadata;
    std::vector<Graph::Vertex> residual_vertices;
    // selection_variables[i] is true iff residual_vertices[i] belongs to T.
    std::vector<std::uint32_t> selection_variables;
    std::size_t cardinality = 0;
};

// Encodes the decision problem cutwidth(G) <= threshold. Every satisfying
// assignment projects to an ordering; auxiliary variables need not be decoded.
[[nodiscard]] CutwidthCnf encode_cutwidth_threshold(
    const Graph& graph, std::uint32_t threshold, EncodingOptions options = {});

[[nodiscard]] CutProfileCnf encode_fixed_prefix_cut_profile(
    const Graph& graph, std::span<const Graph::Mask> prefix,
    std::size_t cardinality, std::uint32_t threshold,
    CardinalityEncoding encoding = CardinalityEncoding::sequential_counter);

// Canonical DIMACS: clauses and literals retain deterministic construction
// order, each line ends in " 0", and the document ends in a newline.
[[nodiscard]] std::string to_dimacs(const CnfFormula& formula);

// Assignment is indexed by DIMACS variable number: assignment[0] is ignored.
// Throws when a required prefix variable is absent or the projected prefixes
// do not describe a permutation.
// Values are DIMACS-indexed and tri-state: -1 is absent, 0 is false, 1 is
// true. Every prefix variable must be assigned explicitly.
[[nodiscard]] std::vector<Graph::Vertex> decode_ordering(
    const CutwidthCnf& encoding, const std::vector<std::int8_t>& assignment);

[[nodiscard]] std::vector<Graph::Vertex> decode_cut_profile_subset(
    const CutProfileCnf& encoding, const std::vector<std::int8_t>& assignment);

[[nodiscard]] bool verify_cut_profile_subset(
    const Graph& graph, std::span<const Graph::Mask> prefix,
    std::size_t cardinality, std::uint32_t threshold,
    std::span<const Graph::Vertex> subset);

// Independent witness checks. verify_ordering checks both permutation and
// advertised threshold; ordering_cutwidth delegates only the graph arithmetic.
[[nodiscard]] std::uint32_t ordering_cutwidth(
    const Graph& graph, const std::vector<Graph::Vertex>& ordering);
[[nodiscard]] bool verify_ordering(
    const Graph& graph, const std::vector<Graph::Vertex>& ordering,
    std::uint32_t threshold);

} // namespace cutwidth::pb
