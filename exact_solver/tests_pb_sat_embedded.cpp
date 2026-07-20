#include "graph.hpp"
#include "pb_cadical_incremental.hpp"
#include "pb_drat_trim_adapter.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cutwidth;
using namespace cutwidth::pb;

extern "C" int run_drat_trim_in_memory(
    const unsigned char*, std::size_t, const unsigned char*, std::size_t,
    int, int (*)(void*), void*);

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checker_fail_closed_tests() {
    CnfFormula contradiction;
    contradiction.variable_count = 2;
    contradiction.clauses = {{1, 2}, {-1, 2}, {1, -2}, {-1, -2}};
    // First derive unit 1 by RUP, then derive the empty clause.  The input
    // itself has no root units, so malformed proof bytes cannot pass merely
    // because parsing the CNF immediately exposes a contradiction.
    const std::vector<std::uint8_t> valid_binary_proof{'a', 2, 0, 'a', 0};
    const auto verified = verify_proof_in_memory(
        contradiction, {}, valid_binary_proof, std::chrono::seconds(5));
    require(verified.status == DratCheckerStatus::verified,
            "valid in-memory DRAT proof was rejected");

    for (const auto& invalid : std::vector<std::vector<std::uint8_t>>{
             {}, {'a'}, {1, 2, 3}}) {
        const auto rejected = verify_proof_in_memory(
            contradiction, {}, invalid, std::chrono::seconds(5));
        require(rejected.status != DratCheckerStatus::verified,
                "malformed in-memory proof was certified");
    }

    std::atomic<bool> cancelled{true};
    const auto stopped = verify_proof_in_memory(
        contradiction, {}, valid_binary_proof, std::chrono::seconds(5), &cancelled);
    require(stopped.status == DratCheckerStatus::timeout,
            "cancelled checker did not return an inconclusive status");

    const std::string invalid_cnf = "not dimacs\n";
    require(run_drat_trim_in_memory(
                reinterpret_cast<const unsigned char*>(invalid_cnf.data()),
                invalid_cnf.size(), valid_binary_proof.data(), valid_binary_proof.size(),
                5, nullptr, nullptr) != 0,
            "upstream exit(0) error path was confused with verification");
}

#ifdef CUTWIDTH_HAVE_CADICAL
void embedded_cadical_tests() {
    const Graph triangle(3, {{0, 1}, {1, 2}, {0, 2}});
    EncodingOptions options;
    options.cardinality = CardinalityEncoding::totalizer;
    options.channel_positions = true;

    const auto impossible = encode_cutwidth_threshold(triangle, 1, options);
    IncrementalCadicalSession no_session(impossible, 1, {}, true, false);
    require(no_session.available(), "embedded CaDiCaL is unavailable");
    const auto no = no_session.solve(1, std::chrono::seconds(5));
    require(no.status == IncrementalStatus::unsat_exploratory,
            "embedded CaDiCaL did not prove the impossible threshold UNSAT");
    require(!no.proof_bytes.empty(), "embedded CaDiCaL emitted no proof bytes");
    const auto checked = verify_proof_in_memory(
        impossible.formula, no.added_unit_clauses, no.proof_bytes,
        std::chrono::seconds(5));
    require(checked.status == DratCheckerStatus::verified,
            "independent checker rejected embedded CaDiCaL proof");

    auto corrupt = no.proof_bytes;
    corrupt.resize(corrupt.size() / 2);
    const auto rejected = verify_proof_in_memory(
        impossible.formula, no.added_unit_clauses, corrupt,
        std::chrono::seconds(5));
    require(rejected.status != DratCheckerStatus::verified,
            "truncated CaDiCaL proof was certified");

    const auto feasible = encode_cutwidth_threshold(triangle, 2, options);
    IncrementalCadicalSession yes_session(feasible, 2, {}, false, false);
    const auto yes = yes_session.solve(2, std::chrono::seconds(5));
    require(yes.status == IncrementalStatus::sat,
            "embedded CaDiCaL rejected a feasible threshold");
    require(verify_ordering(triangle, decode_ordering(feasible, yes.assignment), 2),
            "embedded SAT witness failed independent ordering verification");
}
#endif

} // namespace

int main() {
    try {
        checker_fail_closed_tests();
#ifdef CUTWIDTH_HAVE_CADICAL
        embedded_cadical_tests();
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
