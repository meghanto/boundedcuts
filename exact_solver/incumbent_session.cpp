#include "incumbent_session.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace cutwidth {
namespace {
using Clock = std::chrono::steady_clock;
}

PersistentIncumbentSession::PersistentIncumbentSession(
    const Graph& graph, std::vector<Graph::Vertex> initial_ordering)
    : graph_(graph), current_ordering_(std::move(initial_ordering)),
      best_ordering_(current_ordering_) {
    if (!graph_.validate_ordering(current_ordering_))
        throw std::invalid_argument("persistent incumbent requires a valid ordering");
    current_width_ = graph_.ordering_cutwidth(current_ordering_);
    best_width_ = current_width_;
    rng_ = 0x6a09e667f3bcc909ULL ^
        (static_cast<std::uint64_t>(graph_.size()) << 32U) ^ graph_.edge_count();
    for (Graph::Vertex v = 0; v < graph_.size(); ++v)
        for (const auto word : graph_.adjacency_words(v))
            rng_ = (rng_ ^ word) * 1099511628211ULL;
    if (rng_ == 0) rng_ = 0x9e3779b97f4a7c15ULL;
}

PersistentIncumbentSession::PersistentIncumbentSession(
    const Graph& graph, const IncumbentSnapshot& snapshot)
    : graph_(graph) {
    validate_snapshot(snapshot);
    current_ordering_ = snapshot.current_ordering;
    best_ordering_ = snapshot.best_ordering;
    current_width_ = snapshot.current_width;
    best_width_ = snapshot.best_width;
    rng_ = snapshot.rng_state;
    operator_cursor_ = snapshot.operator_cursor;
    destroy_scale_ = snapshot.destroy_scale;
    if (snapshot.repair) repair_ = restore_repair(*snapshot.repair);
    stats_ = snapshot.stats;
}

PersistentIncumbentSession::RepairState PersistentIncumbentSession::restore_repair(
    const IncumbentRepairSnapshot& snapshot) {
    return {snapshot.kept, snapshot.pending, snapshot.vertex, snapshot.next_position,
            snapshot.best, snapshot.best_profile, snapshot.best_width};
}

IncumbentRepairSnapshot PersistentIncumbentSession::snapshot_repair(
    const RepairState& state) const {
    return {state.kept, state.pending, state.vertex, state.next_position,
            state.best, state.best_profile, state.best_width};
}

void PersistentIncumbentSession::validate_snapshot(
    const IncumbentSnapshot& snapshot) const {
    if (!graph_.validate_ordering(snapshot.current_ordering) ||
        !graph_.validate_ordering(snapshot.best_ordering))
        throw std::invalid_argument("incumbent snapshot has an invalid ordering");
    if (snapshot.current_width != graph_.ordering_cutwidth(snapshot.current_ordering) ||
        snapshot.best_width != graph_.ordering_cutwidth(snapshot.best_ordering) ||
        snapshot.best_width > snapshot.current_width)
        throw std::invalid_argument("incumbent snapshot has inconsistent widths");
    if (snapshot.rng_state == 0 || snapshot.operator_cursor >= 4 ||
        snapshot.destroy_scale == 0 ||
        (!snapshot.current_ordering.empty() &&
         snapshot.destroy_scale > snapshot.current_ordering.size()) ||
        snapshot.stats.service_seconds < 0.0)
        throw std::invalid_argument("incumbent snapshot has invalid policy state");
    if (!snapshot.repair) return;

    const auto& repair = *snapshot.repair;
    std::vector<Graph::Vertex> remaining = repair.kept;
    remaining.push_back(repair.vertex);
    remaining.insert(remaining.end(), repair.pending.begin(), repair.pending.end());
    if (!graph_.validate_ordering(remaining) ||
        repair.next_position > repair.kept.size())
        throw std::invalid_argument("incumbent snapshot has invalid repair state");

    const bool new_vertex = repair.next_position == 0 && repair.best.empty() &&
        repair.best_profile.empty() &&
        repair.best_width == std::numeric_limits<std::uint32_t>::max();
    if (new_vertex) return;
    if (!graph_.validate_ordering(repair.best) ||
        repair.best_width != graph_.ordering_cutwidth(repair.best) ||
        repair.best_profile != cut_profile(repair.best))
        throw std::invalid_argument("incumbent snapshot has invalid repair candidate");
}

IncumbentSnapshot PersistentIncumbentSession::snapshot() const {
    IncumbentSnapshot snapshot;
    snapshot.current_ordering = current_ordering_;
    snapshot.best_ordering = best_ordering_;
    snapshot.current_width = current_width_;
    snapshot.best_width = best_width_;
    snapshot.rng_state = rng_;
    snapshot.operator_cursor = operator_cursor_;
    snapshot.destroy_scale = destroy_scale_;
    if (repair_) snapshot.repair = snapshot_repair(*repair_);
    snapshot.stats = stats_;
    return snapshot;
}

std::uint64_t PersistentIncumbentSession::next_random() noexcept {
    rng_ ^= rng_ >> 12U;
    rng_ ^= rng_ << 25U;
    rng_ ^= rng_ >> 27U;
    return rng_ * 2685821657736338717ULL;
}

std::vector<std::uint32_t> PersistentIncumbentSession::cut_profile(
    const std::vector<Graph::Vertex>& ordering) const {
    std::vector<std::uint32_t> cuts(ordering.size(), 0);
    std::vector<Graph::Mask> prefix(graph_.word_count(), 0);
    for (std::size_t i = 0; i < ordering.size(); ++i) {
        prefix[ordering[i] / 64U] |= Graph::Mask{1} << (ordering[i] % 64U);
        cuts[i] = graph_.cut(prefix);
    }
    std::sort(cuts.begin(), cuts.end(), std::greater<>());
    return cuts;
}

std::vector<std::size_t> PersistentIncumbentSession::choose_destroy_positions() {
    const auto n = current_ordering_.size();
    if (n == 0) return {};
    const auto count = std::min(destroy_scale_, n);
    std::vector<std::size_t> positions;
    positions.reserve(count);
    if (operator_cursor_ == 0 || operator_cursor_ == 2) {
        const auto profile = cut_profile(current_ordering_);
        const auto peak = profile.empty() ? 0U : profile.front();
        std::vector<std::pair<std::uint32_t, std::size_t>> ranked;
        ranked.reserve(n);
        std::vector<Graph::Mask> prefix(graph_.word_count(), 0);
        for (std::size_t i = 0; i < n; ++i) {
            const auto vertex = current_ordering_[i];
            prefix[vertex / 64U] |= Graph::Mask{1} << (vertex % 64U);
            const auto cut = graph_.cut(prefix);
            const auto distance = peak >= cut ? peak - cut : 0;
            const auto score = operator_cursor_ == 0
                ? std::numeric_limits<std::uint32_t>::max() - distance
                : graph_.degree(vertex) + cut;
            ranked.emplace_back(score, i);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.first > b.first || (a.first == b.first && a.second < b.second);
        });
        for (std::size_t i = 0; i < count; ++i) positions.push_back(ranked[i].second);
    } else if (operator_cursor_ == 1) {
        const auto start = static_cast<std::size_t>(next_random() % n);
        for (std::size_t i = 0; i < count; ++i) positions.push_back((start + i) % n);
    } else {
        while (positions.size() < count) {
            const auto position = static_cast<std::size_t>(next_random() % n);
            if (std::find(positions.begin(), positions.end(), position) == positions.end())
                positions.push_back(position);
        }
    }
    std::sort(positions.begin(), positions.end());
    return positions;
}

void PersistentIncumbentSession::begin_repair() {
    const auto positions = choose_destroy_positions();
    RepairState state;
    std::vector<bool> removed(current_ordering_.size(), false);
    for (const auto position : positions) removed[position] = true;
    state.kept.reserve(current_ordering_.size());
    state.pending.reserve(positions.size());
    for (std::size_t i = 0; i < current_ordering_.size(); ++i)
        (removed[i] ? state.pending : state.kept).push_back(current_ordering_[i]);
    if (state.pending.empty()) {
        finish_repaired_candidate(state.kept);
        return;
    }
    state.vertex = state.pending.front();
    state.pending.erase(state.pending.begin());
    repair_ = std::move(state);
}

bool PersistentIncumbentSession::repair_step() {
    if (!repair_) begin_repair();
    if (!repair_) return true;
    auto& state = *repair_;
    auto trial = state.kept;
    trial.insert(trial.begin() + static_cast<std::ptrdiff_t>(state.next_position),
                 state.vertex);
    trial.insert(trial.end(), state.pending.begin(), state.pending.end());
    const auto width = graph_.ordering_cutwidth(trial);
    ++stats_.candidate_evaluations;
    if (width < state.best_width) {
        state.best_width = width;
        state.best = std::move(trial);
        state.best_profile = cut_profile(state.best);
    } else if (width == state.best_width) {
        auto profile = cut_profile(trial);
        if (profile < state.best_profile) {
            state.best = std::move(trial);
            state.best_profile = std::move(profile);
        }
    }
    ++state.next_position;
    if (state.next_position <= state.kept.size()) return false;

    const auto placed_count = state.best.size() - state.pending.size();
    state.kept.assign(state.best.begin(),
        state.best.begin() + static_cast<std::ptrdiff_t>(placed_count));
    if (!state.pending.empty()) {
        state.vertex = state.pending.front();
        state.pending.erase(state.pending.begin());
        state.next_position = 0;
        state.best.clear();
        state.best_profile.clear();
        state.best_width = std::numeric_limits<std::uint32_t>::max();
        return false;
    }
    auto candidate = std::move(state.kept);
    repair_.reset();
    finish_repaired_candidate(std::move(candidate));
    return true;
}

void PersistentIncumbentSession::finish_repaired_candidate(
    std::vector<Graph::Vertex> candidate) {
    if (!graph_.validate_ordering(candidate))
        throw std::logic_error("persistent incumbent repair produced invalid ordering");
    const auto width = graph_.ordering_cutwidth(candidate);
    ++stats_.candidate_evaluations;
    const auto candidate_profile = cut_profile(candidate);
    const auto current_profile = cut_profile(current_ordering_);
    if (width < current_width_ ||
        (width == current_width_ && candidate_profile < current_profile)) {
        current_ordering_ = candidate;
        current_width_ = width;
        ++stats_.accepted_moves;
    }
    if (width < best_width_) {
        best_ordering_ = std::move(candidate);
        best_width_ = graph_.ordering_cutwidth(best_ordering_);
        ++stats_.verified_improvements;
    }
    advance_policy();
    ++stats_.iterations;
}

void PersistentIncumbentSession::advance_policy() noexcept {
    operator_cursor_ = (operator_cursor_ + 1U) % 4U;
    if (operator_cursor_ == 0) {
        if (destroy_scale_ >= current_ordering_.size() ||
            destroy_scale_ > std::numeric_limits<std::size_t>::max() / 2U)
            destroy_scale_ = 1;
        else
            destroy_scale_ = std::min(current_ordering_.size(), destroy_scale_ * 2U);
    }
}

IncumbentServiceResult PersistentIncumbentSession::service(
    std::uint64_t maximum_work_units, Clock::time_point deadline) {
    const auto started = Clock::now();
    ++stats_.service_calls;
    const auto width_before = best_width_;
    const auto iterations_before = stats_.iterations;
    std::uint64_t work_completed = 0;
    while (work_completed < maximum_work_units && Clock::now() < deadline) {
        (void)repair_step();
        ++work_completed;
    }
    const bool improved = best_width_ < width_before;
    if (!improved) ++stats_.no_progress_bursts;
    stats_.service_seconds += std::chrono::duration<double>(Clock::now() - started).count();
    return {improved, Clock::now() >= deadline, best_width_, best_ordering_,
            stats_.iterations - iterations_before, work_completed};
}

} // namespace cutwidth
