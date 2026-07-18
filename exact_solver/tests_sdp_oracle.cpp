#include "sdp_bound_oracle.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

using cutwidth::Graph;
using namespace cutwidth::sdp;

namespace {

Graph path3() {
    return Graph(3, {{0, 1}, {1, 2}});
}

SdpBoundRequest root_request(const Graph& graph) {
    static Graph::Mask empty = 0;
    SdpBoundRequest request;
    request.prefix = std::span<const Graph::Mask>(&empty, graph.word_count());
    request.cardinality = 1;
    request.root = true;
    return request;
}

void test_disabled_and_validation() {
    const auto graph = path3();
    SdpBoundOracle disabled(graph, {});
    assert(disabled.bound(root_request(graph)).status == SdpBoundStatus::disabled);

    SdpBoundOracleOptions options;
    options.schedule = SdpSchedule::root;
    SdpBoundOracle oracle(graph, options);
    auto invalid = root_request(graph);
    invalid.cardinality = 3;
    assert(oracle.bound(invalid).status == SdpBoundStatus::invalid_request);
    Graph::Mask bad = Graph::Mask{1} << 10;
    invalid.prefix = std::span<const Graph::Mask>(&bad, 1);
    invalid.cardinality = 1;
    assert(oracle.bound(invalid).status == SdpBoundStatus::invalid_request);
}

void test_generic_eligibility_and_budgets() {
    const auto graph = path3();
    SdpBoundOracleOptions options;
    options.schedule = SdpSchedule::adaptive;
    options.trigger_nodes = 10;
    options.max_state_dimension = 3;
    SdpBoundOracle oracle(graph, options);
    auto request = root_request(graph);
    request.root = false;
    request.accumulated_subtree_nodes = 9;
    assert(oracle.bound(request).status == SdpBoundStatus::ineligible);
    request.accumulated_subtree_nodes = 10;
    assert(oracle.bound(request).status == SdpBoundStatus::dimension_exceeded);

    options.max_state_dimension = 4;
    SdpBoundOracle unavailable(graph, options);
    assert(!unavailable.should_attempt(4, 9));
    assert(unavailable.should_attempt(4, 10));
    assert(!unavailable.should_attempt(5, 10));
    const auto result = unavailable.bound(request);
#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
    assert(result.status == SdpBoundStatus::certified ||
           result.status == SdpBoundStatus::uncertified);
#else
    assert(result.status == SdpBoundStatus::unavailable);
#endif

    options.total_time = std::chrono::milliseconds{1};
    SdpBoundOracle expired(graph, options);
    request.caller_deadline = std::chrono::steady_clock::now();
    assert(expired.bound(request).status == SdpBoundStatus::time_budget_exhausted);
}

void test_hashes_bind_graph_and_state() {
    const auto graph = path3();
    SdpBoundOracleOptions options;
    options.schedule = SdpSchedule::root;
    options.max_state_dimension = 4;
    SdpBoundOracle oracle(graph, options);
    auto first = oracle.bound(root_request(graph));
    auto second_request = root_request(graph);
    Graph::Mask prefix = 1;
    second_request.prefix = std::span<const Graph::Mask>(&prefix, 1);
    second_request.root = false;
    // Root-only scheduling still computes the key before rejecting the call.
    const auto second = oracle.bound(second_request);
    assert(!first.graph_hash.empty());
    assert(first.graph_hash == second.graph_hash);
    assert(first.model_hash != second.model_hash);
}

} // namespace

int main() {
    test_disabled_and_validation();
    test_generic_eligibility_and_budgets();
    test_hashes_bind_graph_and_state();
    std::cout << "sdp oracle tests passed\n";
}
