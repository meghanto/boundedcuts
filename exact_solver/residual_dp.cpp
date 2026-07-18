#include "residual_dp.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace cutwidth {

std::optional<ResidualDpProjection> project_residual_dp(
    std::size_t residual, std::size_t words) noexcept {
    constexpr auto bits = std::numeric_limits<std::size_t>::digits;
    if (residual >= bits) return std::nullopt;
    const std::size_t states = std::size_t{1} << residual;
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (states > maximum / sizeof(std::uint32_t) ||
        words > maximum / sizeof(Graph::Mask) ||
        residual > maximum / sizeof(Graph::Vertex)) return std::nullopt;
    const auto table = states * sizeof(std::uint32_t);
    const auto word_bytes = words * sizeof(Graph::Mask);
    const auto vertex_bytes = residual * sizeof(Graph::Vertex);
    if (word_bytes > maximum - vertex_bytes || table > maximum - word_bytes - vertex_bytes)
        return std::nullopt;
    return ResidualDpProjection{residual, states, table,
        word_bytes + vertex_bytes, table + word_bytes + vertex_bytes};
}

namespace {
bool contains(std::span<const Graph::Mask> words, Graph::Vertex vertex) {
    return (words[vertex / 64U] & (Graph::Mask{1} << (vertex % 64U))) != 0;
}

bool same_projection(const std::optional<ResidualDpProjection>& left,
                     const std::optional<ResidualDpProjection>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    return left->residual_vertices == right->residual_vertices &&
           left->states == right->states &&
           left->table_bytes == right->table_bytes &&
           left->workspace_bytes == right->workspace_bytes &&
           left->peak_bytes == right->peak_bytes;
}

bool valid_prefix_for_graph(const Graph& graph,
                            std::span<const Graph::Mask> prefix) {
    if (prefix.size() != graph.word_count()) return false;
    if (prefix.empty() || graph.size() % 64U == 0) return true;
    const auto used = graph.size() % 64U;
    const auto allowed = (Graph::Mask{1} << used) - 1U;
    return (prefix.back() & ~allowed) == 0;
}

std::vector<Graph::Vertex> remaining_for(const Graph& graph,
                                         std::span<const Graph::Mask> prefix) {
    std::vector<Graph::Vertex> remaining;
    remaining.reserve(graph.size());
    for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex)
        if (!contains(prefix, vertex)) remaining.push_back(vertex);
    return remaining;
}

std::uint32_t table_value_for(const Graph& graph,
                              std::span<const Graph::Mask> initial_prefix,
                              std::span<const Graph::Vertex> remaining,
                              std::span<const std::uint32_t> table,
                              std::size_t subset) {
    std::vector<Graph::Mask> prefix(initial_prefix.begin(), initial_prefix.end());
    for (std::size_t local = 0; local < remaining.size(); ++local)
        if ((subset >> local) & 1U)
            prefix[remaining[local] / 64U] |=
                Graph::Mask{1} << (remaining[local] % 64U);
    const auto cut = graph.cut(prefix);
    std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
    auto choices = subset;
    while (choices) {
        const auto bit = choices & (~choices + 1U);
        choices -= bit;
        best = std::min(best, std::max(table[subset ^ bit], cut));
    }
    return best;
}
}

ResidualDpSession::ResidualDpSession(
    const Graph& graph, std::span<const Graph::Mask> prefix,
    std::shared_ptr<MemoryGovernor> governor)
    : graph_(graph), initial_prefix_(prefix.begin(), prefix.end()),
      governor_(std::move(governor)) {
    if (!governor_) throw std::invalid_argument("residual DP requires a memory governor");
    if (initial_prefix_.size() != graph.word_count())
        throw std::invalid_argument("residual DP prefix has wrong word count");
    for (Graph::Vertex v = 0; v < graph.size(); ++v)
        if (!contains(initial_prefix_, v)) remaining_.push_back(v);
    projection_ = project_residual_dp(remaining_.size(), graph.word_count());
    if (!projection_) return;
    lease_ = governor_->try_acquire("residual-dp", projection_->peak_bytes);
    if (!lease_) return;
    try {
        table_.assign(projection_->states, std::numeric_limits<std::uint32_t>::max());
    } catch (const std::bad_alloc&) {
        lease_.reset();
        return;
    }
    table_[0] = graph_.cut(initial_prefix_);
    applicable_ = true;
    if (projection_->states == 1) complete_ = true;
}

ResidualDpEvent ResidualDpSession::service(
    std::uint64_t work_units, std::chrono::steady_clock::time_point deadline) {
    ResidualDpEvent event;
    event.applicable = applicable_;
    event.peak_bytes = projection_ ? projection_->peak_bytes : 0;
    if (!applicable_) return event;
    if (complete_) {
        event.complete = true;
        event.exact_completion = table_.back();
        return event;
    }
    const auto target = work_units == 0 ? 1 : work_units;
    std::vector<Graph::Mask> prefix(initial_prefix_.size());
    while (next_state_ < table_.size() && event.states_completed < target) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        prefix = initial_prefix_;
        auto subset = next_state_;
        for (std::size_t local = 0; local < remaining_.size(); ++local)
            if ((subset >> local) & 1U)
                prefix[remaining_[local] / 64U] |=
                    Graph::Mask{1} << (remaining_[local] % 64U);
        const auto cut = graph_.cut(prefix);
        std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
        auto choices = subset;
        while (choices) {
            const auto bit = choices & (~choices + 1U);
            choices -= bit;
            best = std::min(best, std::max(table_[subset ^ bit], cut));
        }
        table_[next_state_++] = best;
        ++event.states_completed;
    }
    if (next_state_ == table_.size()) complete_ = true;
    event.complete = complete_;
    if (complete_) event.exact_completion = table_.back();
    return event;
}

std::vector<Graph::Vertex> ResidualDpSession::reconstruct_witness() const {
    if (!applicable_)
        throw std::logic_error("reconstruct_witness: residual DP was not applicable");
    if (!complete_)
        throw std::logic_error("reconstruct_witness: residual DP table is incomplete");

    const auto n = remaining_.size();
    // Trace backward from the full subset to the empty subset.
    // At each step, select the vertex whose removal reproduces the stored
    // optimal value via the recurrence.
    std::vector<Graph::Vertex> placement;
    placement.reserve(n);
    std::size_t subset = table_.size() - 1U;  // full set of remaining vertices

    for (std::size_t placed = n; placed-- > 0;) {
        // Recompute the cut for this subset to validate the recurrence choice.
        std::vector<Graph::Mask> prefix(initial_prefix_.begin(), initial_prefix_.end());
        for (std::size_t local = 0; local < n; ++local)
            if ((subset >> local) & 1U)
                prefix[remaining_[local] / 64U] |=
                    Graph::Mask{1} << (remaining_[local] % 64U);
        const auto cut = graph_.cut(prefix);
        const auto target = table_[subset];

        // Find the vertex that was placed last: removing it must yield the
        // stored value via max(table_[subset ^ bit], cut) == target.
        bool found = false;
        auto choices = subset;
        while (choices) {
            const auto bit = choices & (~choices + 1U);
            choices -= bit;
            if (std::max(table_[subset ^ bit], cut) == target) {
                const auto local = static_cast<std::size_t>(std::countr_zero(static_cast<std::size_t>(bit)));
                placement.push_back(remaining_[local]);
                subset ^= bit;
                found = true;
                break;
            }
        }
        if (!found)
            throw std::logic_error("reconstruct_witness: recurrence trace failed at popcount " +
                                   std::to_string(placed + 1));
    }

    // placement is in reverse order (last placed first). Reverse for forward
    // placement order.
    std::reverse(placement.begin(), placement.end());
    return placement;
}

ResidualDpSnapshot ResidualDpSession::snapshot() const {
    return ResidualDpSnapshot{initial_prefix_, remaining_, projection_, table_,
                              next_state_, applicable_, complete_};
}

ResidualDpSession ResidualDpSession::restore(
    const Graph& graph, const ResidualDpSnapshot& snapshot,
    std::shared_ptr<MemoryGovernor> governor) {
    if (!governor) throw std::invalid_argument("residual DP restore requires a memory governor");
    if (!valid_prefix_for_graph(graph, snapshot.initial_prefix))
        throw std::invalid_argument("residual DP snapshot prefix does not match graph");
    const auto expected_remaining = remaining_for(graph, snapshot.initial_prefix);
    if (snapshot.remaining != expected_remaining)
        throw std::invalid_argument("residual DP snapshot remaining vertices do not match prefix");
    const auto expected_projection =
        project_residual_dp(snapshot.remaining.size(), graph.word_count());
    if (!same_projection(snapshot.projection, expected_projection))
        throw std::invalid_argument("residual DP snapshot projection does not match graph");

    const auto uncomputed = std::numeric_limits<std::uint32_t>::max();
    if (!snapshot.applicable) {
        if (!snapshot.table.empty() || snapshot.next_state != 1 || snapshot.complete)
            throw std::invalid_argument("inapplicable residual DP snapshot has live state");
    } else {
        if (!snapshot.projection || snapshot.table.size() != snapshot.projection->states ||
            snapshot.next_state == 0 || snapshot.next_state > snapshot.table.size() ||
            snapshot.complete != (snapshot.next_state == snapshot.table.size()) ||
            snapshot.table.empty() ||
            snapshot.table.front() != graph.cut(snapshot.initial_prefix))
            throw std::invalid_argument("residual DP snapshot table shape is invalid");
        for (std::size_t state = 1; state < snapshot.next_state; ++state)
            if (snapshot.table[state] != table_value_for(
                    graph, snapshot.initial_prefix, snapshot.remaining,
                    snapshot.table, state))
                throw std::invalid_argument("residual DP snapshot table disagrees with graph");
        for (std::size_t state = snapshot.next_state; state < snapshot.table.size(); ++state)
            if (snapshot.table[state] != uncomputed)
                throw std::invalid_argument("residual DP snapshot has invalid uncomputed table state");
    }

    ResidualDpSession restored(graph, snapshot.initial_prefix, std::move(governor));
    if (!same_projection(restored.projection_, snapshot.projection))
        throw std::invalid_argument("residual DP restore projection changed");
    if (snapshot.applicable && !restored.applicable_)
        throw std::runtime_error("residual DP restore could not reacquire memory lease");
    if (!snapshot.applicable) {
        restored.lease_.reset();
        restored.table_.clear();
        restored.next_state_ = 1;
        restored.applicable_ = false;
        restored.complete_ = false;
        return restored;
    }
    restored.remaining_ = snapshot.remaining;
    restored.table_ = snapshot.table;
    restored.next_state_ = snapshot.next_state;
    restored.applicable_ = true;
    restored.complete_ = snapshot.complete;
    return restored;
}

} // namespace cutwidth
