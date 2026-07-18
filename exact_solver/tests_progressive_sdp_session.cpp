#include "progressive_sdp_session.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <utility>
#include <vector>

using namespace cutwidth;
using namespace cutwidth::sdp;

namespace {

class RecordingOracle final : public ProgressiveSdpOracle {
public:
    std::vector<std::size_t> calls;
    std::vector<SdpBoundResult> results;

    SdpBoundResult bound(const SdpBoundRequest& request) override {
        calls.push_back(request.cardinality);
        assert(request.caller_deadline != std::chrono::steady_clock::time_point::max());
        assert(!results.empty());
        auto result = results.front();
        results.erase(results.begin());
        return result;
    }
};

ProgressiveSdpTask task(std::uint32_t threshold, std::uint64_t generation,
                        std::size_t cardinality, Graph::Mask prefix = 0) {
    return {{threshold, generation, {prefix}, cardinality}, 7, 2, false};
}

SdpBoundResult result(SdpBoundStatus status, std::optional<std::uint32_t> lb = std::nullopt) {
    SdpBoundResult out;
    out.status = status;
    out.certified_lower_bound = lb;
    out.proof_kind = "test-proof";
    out.graph_hash = "g";
    out.model_hash = "m";
    out.backend_hash = "b";
    return out;
}

void test_certified_only_and_stale_rejection() {
    RecordingOracle oracle;
    oracle.results = {result(SdpBoundStatus::uncertified, 99),
                      result(SdpBoundStatus::certified, 6)};
    ProgressiveSdpSession session(oracle);
    session.activate_threshold(15, 4);
    assert(session.enqueue(task(15, 4, 1)));
    assert(session.enqueue(task(15, 4, 2)));
    assert(!session.enqueue(task(15, 3, 3)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    assert(!session.service_one(deadline).committed);
    assert(!session.certified_lower_bound());
    assert(session.service_one(deadline).committed);
    assert(session.certified_lower_bound() == 6);
    assert(session.committed_records().size() == 1);

    assert(session.enqueue(task(15, 4, 4)));
    session.activate_threshold(15, 5);
    const auto stale = session.service_one(deadline);
    assert(stale.stale_rejected && !stale.status);
    assert(oracle.calls == std::vector<std::size_t>({1, 2}));
}

std::vector<std::size_t> service_order(bool snapshot_between) {
    RecordingOracle oracle;
    oracle.results = {result(SdpBoundStatus::certified, 3), result(SdpBoundStatus::certified, 4),
                      result(SdpBoundStatus::certified, 5)};
    ProgressiveSdpSession session(oracle);
    session.activate_threshold(15, 1);
    session.activate_threshold(12, 2);
    assert(session.enqueue(task(15, 1, 3)));
    assert(session.enqueue(task(12, 2, 2)));
    assert(session.enqueue(task(15, 1, 1)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    (void)session.service_one(deadline);
    if (snapshot_between) {
        const auto snapshot = session.snapshot();
        session.restore(snapshot);
    }
    while (session.has_pending()) (void)session.service_one(deadline);
    return oracle.calls;
}

void test_stable_order_and_snapshot_continuation() {
    const auto uninterrupted = service_order(false);
    const auto resumed = service_order(true);
    assert(uninterrupted == std::vector<std::size_t>({2, 1, 3}));
    assert(resumed == uninterrupted);

    RecordingOracle oracle;
    ProgressiveSdpSession session(oracle);
    session.activate_threshold(10, 1);
    assert(session.enqueue(task(10, 1, 1)));
    const auto expired = session.service_one(std::chrono::steady_clock::now());
    assert(expired.deadline_rejected && oracle.calls.empty());
    assert(session.has_pending() && session.cursor() == 0 && !session.has_running_task());
    oracle.results = {result(SdpBoundStatus::certified, 2)};
    const auto retried = session.service_one(
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    assert(retried.committed && oracle.calls == std::vector<std::size_t>({1}));

    // A stale task is consumed once, while the following queued task remains
    // available in the deterministic order.
    RecordingOracle stale_oracle;
    stale_oracle.results = {result(SdpBoundStatus::certified, 3)};
    ProgressiveSdpSession stale_session(stale_oracle);
    stale_session.activate_threshold(11, 1);
    stale_session.activate_threshold(12, 1);
    assert(stale_session.enqueue(task(11, 1, 1)));
    assert(stale_session.enqueue(task(12, 1, 2)));
    stale_session.deactivate_threshold(11);
    const auto stale = stale_session.service_one(
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    assert(stale.stale_rejected && stale_session.cursor() == 1);
    assert(!stale_session.has_running_task() && stale_session.has_pending());
}

} // namespace

int main() {
    test_certified_only_and_stale_rejection();
    test_stable_order_and_snapshot_continuation();
    std::cout << "progressive SDP session tests passed\n";
}
