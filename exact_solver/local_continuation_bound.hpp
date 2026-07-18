#pragma once

#include "delta_bucket_candidate_index.hpp"
#include "graph.hpp"
#include "vertex_set.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cutwidth {

struct LocalContinuationResult {
    bool continuation_exists = true;
    bool conclusive = true;
    std::uint64_t states = 0;
    std::uint64_t cross_checks = 0;
};

// Read-only finite-horizon proof probe. Hypothetical placements live in a
// tiny overlay; the owning prefix, before-counts, and delta index are never
// mutated. A locally placed neighbour lowers a base delta by exactly two, so
// gathering base buckets through (legal_delta + 2 * overlay_size) is a safe
// superset from which exact legal children are filtered.
class LocalContinuationProbe {
public:
    LocalContinuationProbe(const Graph& graph, std::uint32_t threshold,
                           const VertexSet& prefix,
                           const std::vector<std::uint32_t>& before,
                           const DeltaBucketCandidateIndex& index,
                           const std::vector<std::vector<Graph::Vertex>>& twins,
                           bool cross_check, std::uint64_t maximum_states)
        : graph_(graph), threshold_(threshold), prefix_(prefix), before_(before),
          index_(index), twins_(twins), cross_check_(cross_check),
          maximum_states_(maximum_states) {}

    LocalContinuationResult run(std::uint32_t current_cut,
                                std::uint32_t horizon) {
        overlay_.clear();
        overlay_.reserve(horizon);
        const auto outcome = search(current_cut, horizon);
        result_.continuation_exists = outcome != Outcome::none;
        result_.conclusive = outcome != Outcome::inconclusive;
        return result_;
    }

private:
    struct Candidate {
        Graph::Vertex vertex = 0;
        std::uint32_t cut = 0;
    };
    enum class Outcome { none, exists, inconclusive };

    bool locally_placed(Graph::Vertex vertex) const {
        return std::find(overlay_.begin(), overlay_.end(), vertex) != overlay_.end();
    }

    std::uint32_t local_neighbours(Graph::Vertex vertex) const {
        std::uint32_t count = 0;
        for (const auto placed : overlay_)
            count += static_cast<std::uint32_t>(graph_.adjacent(vertex, placed));
        return count;
    }

    bool twin_blocked(Graph::Vertex vertex) const {
        for (const auto earlier : twins_[vertex])
            if (!prefix_.contains(earlier) && !locally_placed(earlier)) return true;
        return false;
    }

    static bool canonical_less(const Candidate& left,
                               const Candidate& right) {
        if (left.vertex != right.vertex) return left.vertex < right.vertex;
        return left.cut < right.cut;
    }

    std::vector<Candidate> enumerate(std::uint32_t current_cut) {
        std::vector<Candidate> candidates;
        const auto legal_delta = static_cast<std::int64_t>(threshold_) - current_cut;
        const auto superset_delta = std::min<std::int64_t>(
            std::numeric_limits<std::int32_t>::max(),
            legal_delta + 2 * static_cast<std::int64_t>(overlay_.size()));
        index_.gather(static_cast<std::int32_t>(superset_delta),
            [&](Graph::Vertex vertex, std::int32_t base_delta) {
                if (locally_placed(vertex) || twin_blocked(vertex)) return;
                const auto adjusted_delta = static_cast<std::int64_t>(base_delta) -
                    2 * local_neighbours(vertex);
                const auto next = static_cast<std::int64_t>(current_cut) + adjusted_delta;
                if (next >= 0 && next <= static_cast<std::int64_t>(threshold_))
                    candidates.push_back(
                        {vertex, static_cast<std::uint32_t>(next)});
            });

        if (cross_check_) {
            std::vector<Candidate> scanned;
            for (Graph::Vertex vertex = 0; vertex < graph_.size(); ++vertex) {
                if (prefix_.contains(vertex) || locally_placed(vertex) ||
                    twin_blocked(vertex)) continue;
                const auto adjusted_before = before_[vertex] + local_neighbours(vertex);
                const auto next = static_cast<std::int64_t>(current_cut) +
                    graph_.degree(vertex) - 2 * adjusted_before;
                if (next >= 0 && next <= static_cast<std::int64_t>(threshold_))
                    scanned.push_back(
                        {vertex, static_cast<std::uint32_t>(next)});
            }
            auto indexed = candidates;
            std::sort(scanned.begin(), scanned.end(), canonical_less);
            std::sort(indexed.begin(), indexed.end(), canonical_less);
            if (scanned.size() != indexed.size() ||
                !std::equal(scanned.begin(), scanned.end(), indexed.begin(),
                    [](const Candidate& left, const Candidate& right) {
                        return left.vertex == right.vertex &&
                               left.cut == right.cut;
                    }))
                throw std::logic_error(
                    "local continuation candidates differ from full scan");
            ++result_.cross_checks;
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                if (left.cut != right.cut) return left.cut > right.cut;
                return left.vertex < right.vertex;
            });
        return candidates;
    }

    Outcome search(std::uint32_t current_cut, std::uint32_t remaining_steps) {
        if (maximum_states_ != 0 && result_.states >= maximum_states_)
            return Outcome::inconclusive;
        ++result_.states;
        if (prefix_.count() + overlay_.size() == graph_.size() ||
            remaining_steps == 0)
            return Outcome::exists;

        auto candidates = enumerate(current_cut);
        if (candidates.empty()) return Outcome::none;
        bool saw_inconclusive = false;
        for (const auto candidate : candidates) {
            overlay_.push_back(candidate.vertex);
            const auto outcome = search(candidate.cut, remaining_steps - 1U);
            overlay_.pop_back();
            if (outcome == Outcome::exists) return outcome;
            if (outcome == Outcome::inconclusive) saw_inconclusive = true;
        }
        return saw_inconclusive ? Outcome::inconclusive : Outcome::none;
    }

    const Graph& graph_;
    std::uint32_t threshold_ = 0;
    const VertexSet& prefix_;
    const std::vector<std::uint32_t>& before_;
    const DeltaBucketCandidateIndex& index_;
    const std::vector<std::vector<Graph::Vertex>>& twins_;
    bool cross_check_ = false;
    std::uint64_t maximum_states_ = 0;
    std::vector<Graph::Vertex> overlay_;
    LocalContinuationResult result_;
};

} // namespace cutwidth
