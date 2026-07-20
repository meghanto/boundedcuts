#include "pb_encoding.hpp"
#include "pb_solver.hpp"
#include "pb_cadical_incremental.hpp"
#include "pb_drat_trim_adapter.hpp"
#include "pb_backend.hpp"
#include "oracle.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace {
using cutwidth::Graph;
using namespace cutwidth::pb;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Graph graph_from_bits(std::uint32_t n, std::uint64_t bits) {
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    std::uint32_t bit = 0;
    for (Graph::Vertex u = 0; u < n; ++u)
        for (Graph::Vertex v = u + 1; v < n; ++v, ++bit)
            if ((bits >> bit) & 1U) edges.emplace_back(u, v);
    return Graph(n, edges);
}

bool dpll(const CnfFormula& cnf, std::vector<std::int8_t>& values) {
    while (true) {
        bool changed = false;
        for (const auto& clause : cnf.clauses) {
            bool satisfied = false;
            std::int32_t unit = 0;
            std::size_t unknown = 0;
            for (const auto literal : clause) {
                const auto variable = static_cast<std::size_t>(literal < 0 ? -literal : literal);
                if (values[variable] < 0) { unit = literal; ++unknown; }
                else if ((literal > 0) == (values[variable] != 0)) { satisfied = true; break; }
            }
            if (satisfied) continue;
            if (unknown == 0) return false;
            if (unknown == 1) {
                const auto variable = static_cast<std::size_t>(unit < 0 ? -unit : unit);
                const std::int8_t wanted = unit > 0 ? 1 : 0;
                if (values[variable] >= 0 && values[variable] != wanted) return false;
                if (values[variable] < 0) { values[variable] = wanted; changed = true; }
            }
        }
        if (!changed) break;
    }
    std::size_t branch = 1;
    while (branch < values.size() && values[branch] >= 0) ++branch;
    if (branch == values.size()) return true;
    for (const std::int8_t value : {std::int8_t{0}, std::int8_t{1}}) {
        auto trial = values;
        trial[branch] = value;
        if (dpll(cnf, trial)) { values = std::move(trial); return true; }
    }
    return false;
}

std::uint32_t exact_cut_profile(
    const Graph& graph, Graph::Mask prefix, std::size_t cardinality) {
    std::vector<Graph::Vertex> residual;
    for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex)
        if ((prefix & (Graph::Mask{1} << vertex)) == 0)
            residual.push_back(vertex);
    std::vector<bool> selected(residual.size(), false);
    std::fill(selected.end() - static_cast<std::ptrdiff_t>(cardinality),
              selected.end(), true);
    auto best = std::numeric_limits<std::uint32_t>::max();
    do {
        auto combined = prefix;
        for (std::size_t i = 0; i < residual.size(); ++i)
            if (selected[i]) combined |= Graph::Mask{1} << residual[i];
        best = std::min(best, graph.cut(combined));
    } while (std::next_permutation(selected.begin(), selected.end()));
    return best;
}

void exhaustive_cut_profile_encoding_tests() {
    for (std::uint32_t n = 2; n <= 4; ++n) {
        const auto pairs = n * (n - 1) / 2;
        const auto graph_count = std::uint64_t{1} << pairs;
        const auto prefix_count = Graph::Mask{1} << n;
        for (std::uint64_t bits = 0; bits < graph_count; ++bits) {
            const auto graph = graph_from_bits(n, bits);
            for (Graph::Mask prefix = 0; prefix < prefix_count; ++prefix) {
                const auto residual = n - std::popcount(prefix);
                if (residual <= 1) continue;
                const std::vector<Graph::Mask> words{prefix};
                for (std::size_t cardinality = 1; cardinality < residual;
                     ++cardinality) {
                    const auto exact = exact_cut_profile(
                        graph, prefix, cardinality);
                    for (const auto threshold : {
                             exact == 0 ? std::uint32_t{0} : exact - 1U, exact}) {
                        for (const auto kind : {
                                 CardinalityEncoding::sequential_counter,
                                 CardinalityEncoding::totalizer}) {
                            const auto encoded = encode_fixed_prefix_cut_profile(
                                graph, words, cardinality, threshold, kind);
                            require(encoded.metadata.model ==
                                        "fixed-prefix-cut-profile-cnf-v1",
                                    "cut-profile metadata model is wrong");
                            require(!encoded.metadata.dimacs_fnv1a64.empty(),
                                    "cut-profile model hash is empty");
                            std::vector<std::int8_t> assignment(
                                encoded.formula.variable_count + 1, -1);
                            const bool satisfiable = dpll(
                                encoded.formula, assignment);
                            require(satisfiable == (exact <= threshold),
                                    "cut-profile CNF disagrees with enumeration");
                            if (satisfiable) {
                                const auto subset = decode_cut_profile_subset(
                                    encoded, assignment);
                                require(verify_cut_profile_subset(
                                            graph, words, cardinality,
                                            threshold, subset),
                                        "cut-profile SAT witness failed verification");
                            }
                        }
                    }
                }
            }
        }
    }
}

void exhaustive_encoding_tests() {
    std::uint32_t maximum_n = 5;
    std::uint64_t shard_index = 0, shard_count = 1;
    if (const char* value = std::getenv("CUTWIDTH_PB_EXHAUSTIVE_N"))
        maximum_n = static_cast<std::uint32_t>(std::stoul(value));
    if (const char* value = std::getenv("CUTWIDTH_PB_SHARD_INDEX"))
        shard_index = std::stoull(value);
    if (const char* value = std::getenv("CUTWIDTH_PB_SHARD_COUNT"))
        shard_count = std::stoull(value);
    if (maximum_n > 6 || shard_count == 0 || shard_index >= shard_count)
        throw std::invalid_argument("invalid PB exhaustive-test shard configuration");
    if (maximum_n == 6 && shard_count < 2)
        throw std::invalid_argument("n=6 PB sweep must be split into at least two shards");
    for (std::uint32_t n = 0; n <= maximum_n; ++n) {
        const auto pairs = n * (n - 1) / 2;
        const std::uint64_t count = std::uint64_t{1} << pairs;
        const auto first = n == 6 ? shard_index : 0;
        const auto stride = n == 6 ? shard_count : 1;
        for (std::uint64_t bits = first; bits < count; bits += stride) {
            const auto graph = graph_from_bits(n, bits);
            const auto optimum = cutwidth::oracle::subset_dp(graph).cutwidth;
            const std::uint32_t maximum_threshold = std::max<std::uint32_t>(optimum, 1);
            for (std::uint32_t threshold = 0; threshold <= maximum_threshold; ++threshold) {
                for (const auto kind : {CardinalityEncoding::sequential_counter,
                                        CardinalityEncoding::totalizer}) {
                    for (const bool symmetry : {false, true}) {
                      for (const bool channel_positions : {false, true}) {
                        EncodingOptions options;
                        options.cardinality = kind;
                        options.break_reversal_symmetry = symmetry;
                        options.channel_positions = channel_positions;
                        const auto encoded = encode_cutwidth_threshold(graph, threshold, options);
                        require(encoded.metadata.threshold == threshold,
                                "PB metadata has wrong threshold");
                        require(!encoded.metadata.dimacs_fnv1a64.empty(),
                                "PB model hash is empty");
                        std::vector<std::int8_t> assignment(
                            encoded.formula.variable_count + 1, -1);
                        const bool satisfiable = dpll(encoded.formula, assignment);
                        require(satisfiable == (optimum <= threshold),
                                "PB encoding disagrees with exact cutwidth");
                        if (satisfiable) {
                            const auto ordering = decode_ordering(encoded, assignment);
                            require(verify_ordering(graph, ordering, threshold),
                                    "decoded PB witness failed independent verification");
                        }
                      }
                    }
                }
            }
        }
    }
}

void malformed_tests() {
    const Graph edge(2, {{0, 1}});
    auto encoded = encode_cutwidth_threshold(edge, 1);
    bool rejected = false;
    try { (void)decode_ordering(encoded, std::vector<std::int8_t>(1, -1)); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "truncated SAT assignment was accepted");
    auto malformed = encoded.formula;
    malformed.clauses.push_back({0});
    rejected = false;
    try { (void)to_dimacs(malformed); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "zero DIMACS literal was accepted");
    require(!verify_ordering(edge, {0, 0}, 1), "duplicate witness was accepted");
}

void external_adapter_tests() {
    CnfFormula formula{1, {{1}}};
    ExternalSatOptions unavailable;
    unavailable.solver_path = "/definitely/not/a/solver";
    require(solve_dimacs_external(formula, unavailable).status == ExternalSatStatus::unavailable,
            "missing PB backend was not reported unavailable");
#if defined(__unix__) || defined(__APPLE__)
    const auto base = std::filesystem::temp_directory_path() / "cutwidth-pb-tests";
    std::filesystem::create_directories(base);
    auto script = [&](const char* name, const char* body) {
        const auto path = base / name;
        std::ofstream out(path); out << "#!/bin/sh\n" << body << '\n'; out.close();
        ::chmod(path.c_str(), 0700);
        return path.string();
    };
    ExternalSatOptions malformed;
    malformed.solver_path = script("malformed.sh",
        "[ \"$1\" = \"--version\" ] && { echo fake-1; exit 0; }; echo 's SATISFIABLE'; echo 'v 2 0'; exit 10");
    require(solve_dimacs_external(formula, malformed).status == ExternalSatStatus::invalid_output,
            "out-of-range SAT model was accepted");
    ExternalSatOptions sat;
    sat.solver_path = script("sat.sh",
        "[ \"$1\" = \"--version\" ] && { echo fake-1; exit 0; }; echo 's SATISFIABLE'; echo 'v 1 0'; exit 10");
    const auto sat_result = solve_dimacs_external(formula, sat);
    require(sat_result.status == ExternalSatStatus::sat &&
            sat_result.assignment.size() == 2 && sat_result.assignment[1] == 1,
            "valid SAT model was not parsed");
    ExternalSatOptions unproved;
    unproved.solver_path = script("unsat.sh",
        "[ \"$1\" = \"--version\" ] && { echo fake-1; exit 0; }; echo 's UNSATISFIABLE'; exit 20");
    require(solve_dimacs_external(formula, unproved).status == ExternalSatStatus::unsat_unverified,
            "UNSAT without checked proof was accepted");
    ExternalSatOptions timeout;
    timeout.solver_path = script("timeout.sh",
        "[ \"$1\" = \"--version\" ] && { echo fake-1; exit 0; }; sleep 2");
    timeout.time_limit = std::chrono::milliseconds(20);
    require(solve_dimacs_external(formula, timeout).status == ExternalSatStatus::timed_out,
            "PB timeout was not reported");
    std::filesystem::remove_all(base);
#endif
}

void incremental_cadical_tests() {
    const Graph triangle(3, {{0, 1}, {1, 2}, {0, 2}});
    EncodingOptions options;
    options.cardinality = CardinalityEncoding::totalizer;
    options.channel_positions = true;
    const auto encoded = encode_cutwidth_threshold(triangle, 2, options);
    IncrementalCadicalSession session(encoded, 2, {0, 1, 2}, true);
#ifdef CUTWIDTH_HAVE_CADICAL
    require(session.available(), "compiled incremental CaDiCaL is unavailable");
    const auto sat = session.solve(2, std::chrono::seconds(2));
    require(sat.status == IncrementalStatus::sat,
            "incremental CaDiCaL did not solve feasible threshold");
    require(verify_ordering(triangle, decode_ordering(encoded, sat.assignment), 2),
            "incremental CaDiCaL SAT witness failed verification");
    const auto unsat = session.solve(1, std::chrono::seconds(2));
    require(unsat.status == IncrementalStatus::unsat_exploratory,
            "incremental CaDiCaL did not retain tighter threshold");
    require(!unsat.proof_bytes.empty(), "incremental CaDiCaL did not emit a proof");
    const auto checked = verify_proof_in_memory(
        encoded.formula, unsat.added_unit_clauses,
        unsat.proof_bytes, std::chrono::seconds(2));
    require(checked.status == DratCheckerStatus::verified,
            "incremental CaDiCaL proof failed in-memory DRAT checking");

    const auto direct_encoding = encode_cutwidth_threshold(triangle, 1, options);
    IncrementalCadicalSession direct(
        direct_encoding, 1, {0, 1, 2}, true);
    const auto direct_unsat = direct.solve(1, std::chrono::seconds(2));
    require(direct_unsat.status == IncrementalStatus::unsat_exploratory,
            "direct native CaDiCaL UNSAT solve failed");
    const auto direct_checked = verify_proof_in_memory(
        direct_encoding.formula, direct_unsat.added_unit_clauses,
        direct_unsat.proof_bytes, std::chrono::seconds(2));
    require(direct_checked.status == DratCheckerStatus::verified,
            "direct native CaDiCaL proof failed DRAT checking");

    cutwidth::pb::DecisionOptions native_options;
    native_options.solver = SolverKind::cadical;
    native_options.encoding = CardinalityEncoding::totalizer;
    native_options.channel_positions = true;
    native_options.native_incremental = true;
    native_options.external.time_limit = std::chrono::seconds(2);
    const auto native_yes = cutwidth::pb::decide_cutwidth(
        triangle, 2, native_options);
    require(native_yes.decision.status == cutwidth::DecisionStatus::feasible &&
            native_yes.provenance.witness_verified,
            "certified native PB adapter rejected a feasible threshold");
    const auto native_no = cutwidth::pb::decide_cutwidth(
        triangle, 1, native_options);
    require(native_no.decision.status == cutwidth::DecisionStatus::infeasible &&
            native_no.provenance.proof_checked,
            "certified native PB adapter accepted unchecked UNSAT");

    if (const char* checker = std::getenv("CUTWIDTH_DRAT_TRIM")) {
        ExternalSatOptions check_options;
        check_options.proof_checker_path = checker;
        check_options.time_limit = std::chrono::seconds(2);
        const auto invalid_proof = std::filesystem::temp_directory_path() /
            "cutwidth-incremental-proof.drat";
        { std::ofstream output(invalid_proof, std::ios::binary);
          output.write(reinterpret_cast<const char*>(unsat.proof_bytes.data()),
                       static_cast<std::streamsize>(unsat.proof_bytes.size())); }
        const auto external_checked = check_drat_proof_external(
            encoded.formula, unsat.added_unit_clauses,
            invalid_proof.string(), check_options);
        std::filesystem::remove(invalid_proof);
        require(external_checked.checked,
                "external and embedded proof checkers disagreed");
    }
#else
    require(!session.available(), "uncompiled incremental CaDiCaL reported available");
#endif
}
} // namespace

int main() {
    try {
        exhaustive_cut_profile_encoding_tests();
        exhaustive_encoding_tests();
        malformed_tests();
        external_adapter_tests();
        incremental_cadical_tests();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
