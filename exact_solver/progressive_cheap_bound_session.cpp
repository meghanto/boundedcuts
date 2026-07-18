#include "progressive_cheap_bound_session.hpp"
#include "lagrangian_bound.hpp"

#include <algorithm>
#include <stdexcept>

namespace cutwidth {

void ProgressiveCheapBoundSession::activate_threshold(
    std::uint32_t threshold, std::uint64_t generation) {
    live_generations_[threshold] = generation;
}
void ProgressiveCheapBoundSession::deactivate_threshold(std::uint32_t threshold) {
    live_generations_.erase(threshold);
}
bool ProgressiveCheapBoundSession::live(const ProgressiveCheapBoundTaskId& id) const noexcept {
    const auto found = live_generations_.find(id.threshold);
    return found != live_generations_.end() && found->second == id.generation;
}
bool ProgressiveCheapBoundSession::enqueue(const ParallelUnstartedFragment& fragment,
                                           std::uint64_t generation) {
    const auto& saved = fragment.session;
    if (fragment.region_id == 0 || saved.status != SessionStatus::unresolved ||
        saved.unfinished_regions != 1 || saved.external_regions != 0 ||
        !saved.pending.empty() || saved.frames.size() != 1) return false;
    ProgressiveCheapBoundTask task{{saved.threshold, generation, fragment.region_id, saved.path}};
    if (!live(task.id)) return false;
    const auto duplicate = std::find_if(tasks_.begin() + static_cast<std::ptrdiff_t>(cursor_),
        tasks_.end(), [&](const auto& queued) { return queued.id == task.id; });
    if (duplicate != tasks_.end()) return false;
    tasks_.push_back(std::move(task));
    return true;
}
ProgressiveCheapBoundEvent ProgressiveCheapBoundSession::service_one(
    ParallelDecisionSession& forest) {
    auto fragment = forest.donate_and_claim_deepest_unstarted_fragment();
    if (!fragment) {
        fragment = forest.claim_deepest_unstarted_fragment();
    }
    if (!fragment) return {};
    const auto found = live_generations_.find(fragment->session.threshold);
    if (found == live_generations_.end()) {
        (void)forest.release_claimed_unstarted_fragment(*fragment);
        return {};
    }
    std::uint64_t gen = found->second;
    if (!enqueue(*fragment, gen)) {
        (void)forest.release_claimed_unstarted_fragment(*fragment);
        return {};
    }
    auto event = evaluate_claimed_one(*fragment);
    (void)commit_or_release_claimed(forest, *fragment, event);
    return event;
}

ProgressiveCheapBoundEvent ProgressiveCheapBoundSession::evaluate_claimed_one(
    const ParallelUnstartedFragment& fragment) {
    ProgressiveCheapBoundEvent event;
    if (fragment.reservation_id == 0) {
        event.fragment_rejected = true;
        return event;
    }
    tasks_.erase(std::remove_if(tasks_.begin() + static_cast<std::ptrdiff_t>(cursor_), tasks_.end(),
        [this](const ProgressiveCheapBoundTask& t) { return !live(t.id); }), tasks_.end());
    if (!has_pending()) return event;
    auto it = std::find_if(tasks_.begin() + static_cast<std::ptrdiff_t>(cursor_), tasks_.end(),
        [&](const ProgressiveCheapBoundTask& t) {
            return t.id.region_id == fragment.region_id &&
                   t.id.threshold == fragment.session.threshold &&
                   t.id.prefix == fragment.session.path;
        });
    if (it == tasks_.end()) {
        event.fragment_rejected = true;
        return event;
    }
    ProgressiveCheapBoundTask task = *it;
    tasks_.erase(it);
    event.task = task.id;
    std::vector<Graph::Mask> prefix(graph_.word_count(), 0);
    for (const auto vertex : task.id.prefix)
        prefix[static_cast<std::size_t>(vertex) / 64U] |=
            Graph::Mask{1} << (vertex % 64U);
    PartialBoundEvaluator evaluator(graph_, options_, task.id.threshold);
    evaluator.note_session_ceiling_skips(event.stats);
    event.certified_prune = evaluator.exceeds(
        prefix, static_cast<std::uint32_t>(task.id.prefix.size()),
        task.id.threshold, event.stats);

    if (options_.lagrangian_prefix_bound && !event.certified_prune) {
        std::uint32_t current_cut = (graph_.word_count() == 1) ? graph_.cut(prefix[0]) : graph_.cut(prefix);
        std::uint32_t residual_size = static_cast<std::uint32_t>(graph_.size() - task.id.prefix.size());

        bool slack_ok = (current_cut <= task.id.threshold) && ((task.id.threshold - current_cut) <= options_.lagrangian_max_slack);
        bool residual_ok = (residual_size <= options_.lagrangian_max_residual);

        if (!slack_ok) {
            ++event.stats.lagrangian_slack_gate_skips;
        } else if (!residual_ok) {
            ++event.stats.lagrangian_residual_gate_skips;
        } else {
            std::uint32_t mid = residual_size / 2;
            std::vector<std::uint32_t> selected_cardinalities;
            std::vector<int> offsets = {0, 1, -1, 2, -2};
            for (int offset : offsets) {
                int64_t cand = static_cast<int64_t>(mid) + offset;
                if (cand >= 1 && cand <= static_cast<int64_t>(residual_size) - 1) {
                    std::uint32_t val = static_cast<std::uint32_t>(cand);
                    if (std::find(selected_cardinalities.begin(), selected_cardinalities.end(), val) == selected_cardinalities.end()) {
                        selected_cardinalities.push_back(val);
                    }
                }
            }

            LagrangianPrefixBoundEvaluator lagrangian_evaluator(graph_);
            LagrangianTelemetry tel;
            if (graph_.word_count() == 1) {
                tel = lagrangian_evaluator.evaluate(
                    prefix[0],
                    task.id.threshold,
                    selected_cardinalities,
                    options_.lagrangian_denominator
                );
            } else {
                tel = lagrangian_evaluator.evaluate(
                    prefix,
                    task.id.threshold,
                    selected_cardinalities,
                    options_.lagrangian_denominator
                );
            }

            event.lagrangian_evaluated = true;
            event.lagrangian_best_bound = tel.certified_bound;
            event.lagrangian_best_cardinality = tel.best_cardinality;
            event.lagrangian_best_numerator = tel.best_numerator;
            event.lagrangian_best_denominator = tel.best_denominator;

            if (tel.ineligible) {
                ++event.stats.lagrangian_ineligible_gate_skips;
            } else if (tel.overflow) {
                ++event.stats.lagrangian_overflow_gate_skips;
            } else {
                ++event.stats.lagrangian_evaluations;
                event.stats.lagrangian_mincuts += tel.mincuts;
                if (tel.certified_bound > task.id.threshold) {
                    event.certified_prune = true;
                    ++event.stats.lagrangian_certified_prunes;
                }
            }
        }
    }

    return event;
}

bool ProgressiveCheapBoundSession::commit_or_release_claimed(
    ParallelDecisionSession& forest, const ParallelUnstartedFragment& fragment,
    ProgressiveCheapBoundEvent& event) {
    if (!event.task || event.fragment_rejected || event.stale_rejected) {
        const bool released = forest.release_claimed_unstarted_fragment(fragment);
        if (!released) event.fragment_rejected = true;
        return released;
    }
    const bool committed = event.certified_prune
        ? forest.retire_claimed_unstarted_fragment(fragment)
        : forest.release_claimed_unstarted_fragment(fragment);
    if (!committed) event.fragment_rejected = true;
    if (!event.certified_prune) return committed;
    event.certified_prune = committed;
    return committed;
}

ProgressiveCheapBoundSnapshot ProgressiveCheapBoundSession::snapshot() const {
    return {live_generations_, tasks_, cursor_};
}

void ProgressiveCheapBoundSession::restore(const ProgressiveCheapBoundSnapshot& snapshot) {
    if (snapshot.cursor > snapshot.tasks.size())
        throw std::invalid_argument("invalid progressive cheap-bound snapshot cursor");
    live_generations_ = snapshot.live_generations;
    tasks_ = snapshot.tasks;
    cursor_ = snapshot.cursor;
}

} // namespace cutwidth
