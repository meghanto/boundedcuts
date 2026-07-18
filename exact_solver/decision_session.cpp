#include "decision_session.hpp"

#include "delta_bucket_candidate_index.hpp"
#include "decision_cache.hpp"
#include "local_continuation_bound.hpp"
#include "partial_bounds.hpp"
#include "residual_dp.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <deque>
#include <limits>
#include <stdexcept>

namespace cutwidth {
namespace {

struct Candidate {
    Graph::Vertex vertex = 0;
    std::uint32_t cut = 0;
};

using TwinTable = DecisionTwinTable;

bool equal_open(const Graph& graph, Graph::Vertex a, Graph::Vertex b) {
    for (Graph::Vertex v = 0; v < graph.size(); ++v)
        if (v != a && v != b && graph.adjacent(a, v) != graph.adjacent(b, v))
            return false;
    return !graph.adjacent(a, b);
}

bool equal_closed(const Graph& graph, Graph::Vertex a, Graph::Vertex b) {
    if (!graph.adjacent(a, b)) return false;
    for (Graph::Vertex v = 0; v < graph.size(); ++v) {
        const bool av = v == a || graph.adjacent(a, v);
        const bool bv = v == b || graph.adjacent(b, v);
        if (av != bv) return false;
    }
    return true;
}

TwinTable build_twins(const Graph& graph, bool enabled) {
    TwinTable twins(graph.size());
    if (!enabled) return twins;
    for (Graph::Vertex later = 0; later < graph.size(); ++later)
        for (Graph::Vertex earlier = 0; earlier < later; ++earlier)
            if (equal_open(graph, earlier, later) || equal_closed(graph, earlier, later))
                twins[later].push_back(earlier);
    return twins;
}

DecisionStats subtract_stats(const DecisionStats& after, const DecisionStats& before) {
    DecisionStats delta;
    delta.nodes_expanded = after.nodes_expanded - before.nodes_expanded;
    delta.children_rejected_by_cut = after.children_rejected_by_cut - before.children_rejected_by_cut;
    delta.failed_cache_hits = after.failed_cache_hits - before.failed_cache_hits;
    delta.failed_cache_queries = after.failed_cache_queries - before.failed_cache_queries;
    delta.failed_states_recorded = after.failed_states_recorded - before.failed_states_recorded;
    delta.unique_canonical_claims = after.unique_canonical_claims - before.unique_canonical_claims;
    delta.duplicate_ownership_waits = after.duplicate_ownership_waits - before.duplicate_ownership_waits;
    delta.ownership_saturation = after.ownership_saturation - before.ownership_saturation;
    delta.failed_state_bounds_strengthened = after.failed_state_bounds_strengthened -
        before.failed_state_bounds_strengthened;
    delta.failed_state_insertions_skipped = after.failed_state_insertions_skipped -
        before.failed_state_insertions_skipped;
    delta.cache_collisions = after.cache_collisions - before.cache_collisions;
    delta.cache_page_promotions = after.cache_page_promotions - before.cache_page_promotions;
    delta.cache_page_second_chances = after.cache_page_second_chances - before.cache_page_second_chances;
    delta.cache_pages_recycled = after.cache_pages_recycled - before.cache_pages_recycled;
    delta.cache_replacement_admissions = after.cache_replacement_admissions - before.cache_replacement_admissions;
    delta.cache_entries_evicted = after.cache_entries_evicted - before.cache_entries_evicted;
    delta.cache_evicted_depth_sum = after.cache_evicted_depth_sum - before.cache_evicted_depth_sum;
    delta.cache_maximum_evicted_depth = after.cache_maximum_evicted_depth;
    delta.twin_symmetric_children_skipped = after.twin_symmetric_children_skipped - before.twin_symmetric_children_skipped;
    delta.depth_two_lookahead_checks = after.depth_two_lookahead_checks - before.depth_two_lookahead_checks;
    delta.children_rejected_by_depth_two_lookahead = after.children_rejected_by_depth_two_lookahead - before.children_rejected_by_depth_two_lookahead;
    delta.node_state_updates = after.node_state_updates - before.node_state_updates;
    delta.residual_histogram_updates = after.residual_histogram_updates -
        before.residual_histogram_updates;
    delta.partial_bounds.evaluations = after.partial_bounds.evaluations - before.partial_bounds.evaluations;
    delta.partial_bounds.residual_degree_evaluations = after.partial_bounds.residual_degree_evaluations - before.partial_bounds.residual_degree_evaluations;
    delta.partial_bounds.edge_distance_area_evaluations = after.partial_bounds.edge_distance_area_evaluations - before.partial_bounds.edge_distance_area_evaluations;
    delta.partial_bounds.degree_distance_area_evaluations = after.partial_bounds.degree_distance_area_evaluations - before.partial_bounds.degree_distance_area_evaluations;
    delta.partial_bounds.degeneracy_evaluations = after.partial_bounds.degeneracy_evaluations - before.partial_bounds.degeneracy_evaluations;
    delta.partial_bounds.residual_degree_prunes = after.partial_bounds.residual_degree_prunes - before.partial_bounds.residual_degree_prunes;
    delta.partial_bounds.edge_distance_area_prunes = after.partial_bounds.edge_distance_area_prunes - before.partial_bounds.edge_distance_area_prunes;
    delta.partial_bounds.degree_distance_area_prunes = after.partial_bounds.degree_distance_area_prunes - before.partial_bounds.degree_distance_area_prunes;
    delta.partial_bounds.degeneracy_prunes = after.partial_bounds.degeneracy_prunes - before.partial_bounds.degeneracy_prunes;
    delta.partial_bounds.expensive_slack_gate_skips =
        after.partial_bounds.expensive_slack_gate_skips -
        before.partial_bounds.expensive_slack_gate_skips;
    delta.configured_proof_regions_bound = after.configured_proof_regions_bound;
    delta.resolved_proof_regions_bound = after.resolved_proof_regions_bound;
    delta.peak_proof_regions = after.peak_proof_regions;
    delta.suppressed_donations = after.suppressed_donations - before.suppressed_donations;
    delta.residual_dp_attempts = after.residual_dp_attempts - before.residual_dp_attempts;
    delta.residual_dp_admissions = after.residual_dp_admissions - before.residual_dp_admissions;
    delta.residual_dp_governor_or_cap_rejections = after.residual_dp_governor_or_cap_rejections -
        before.residual_dp_governor_or_cap_rejections;
    delta.residual_dp_completed_tails = after.residual_dp_completed_tails -
        before.residual_dp_completed_tails;
    delta.residual_dp_infeasible_prunes = after.residual_dp_infeasible_prunes -
        before.residual_dp_infeasible_prunes;
    delta.residual_dp_feasible_witnesses = after.residual_dp_feasible_witnesses -
        before.residual_dp_feasible_witnesses;
    delta.residual_dp_states = after.residual_dp_states - before.residual_dp_states;
    delta.residual_dp_peak_bytes = after.residual_dp_peak_bytes;
    delta.residual_dp_seconds = after.residual_dp_seconds - before.residual_dp_seconds;
    delta.residual_dp_cold_restarts = after.residual_dp_cold_restarts -
        before.residual_dp_cold_restarts;
    delta.dfs_min_remaining_vertices = after.dfs_min_remaining_vertices;
    delta.best_next_bucket_checks = after.best_next_bucket_checks - before.best_next_bucket_checks;
    delta.best_next_bucket_parent_prunes = after.best_next_bucket_parent_prunes - before.best_next_bucket_parent_prunes;
    delta.best_next_bucket_candidates_avoided = after.best_next_bucket_candidates_avoided - before.best_next_bucket_candidates_avoided;
    delta.candidate_scan_checks = after.candidate_scan_checks - before.candidate_scan_checks;
    delta.candidate_index_gathers = after.candidate_index_gathers - before.candidate_index_gathers;
    delta.candidate_index_bucket_slots_visited = after.candidate_index_bucket_slots_visited -
        before.candidate_index_bucket_slots_visited;
    delta.candidate_index_vertices_emitted = after.candidate_index_vertices_emitted -
        before.candidate_index_vertices_emitted;
    delta.candidate_index_forward_updates = after.candidate_index_forward_updates -
        before.candidate_index_forward_updates;
    delta.candidate_index_rollback_updates = after.candidate_index_rollback_updates -
        before.candidate_index_rollback_updates;
    delta.candidate_index_cross_checks = after.candidate_index_cross_checks -
        before.candidate_index_cross_checks;
    delta.local_continuation_calls = after.local_continuation_calls -
        before.local_continuation_calls;
    delta.local_continuation_slack_gate_skips =
        after.local_continuation_slack_gate_skips -
        before.local_continuation_slack_gate_skips;
    delta.local_continuation_branch_gate_skips =
        after.local_continuation_branch_gate_skips -
        before.local_continuation_branch_gate_skips;
    delta.local_continuation_inconclusive = after.local_continuation_inconclusive -
        before.local_continuation_inconclusive;
    delta.local_continuation_states = after.local_continuation_states -
        before.local_continuation_states;
    delta.local_continuation_parent_prunes =
        after.local_continuation_parent_prunes -
        before.local_continuation_parent_prunes;
    delta.local_continuation_nanoseconds = after.local_continuation_nanoseconds -
        before.local_continuation_nanoseconds;
    delta.local_continuation_cross_checks =
        after.local_continuation_cross_checks -
        before.local_continuation_cross_checks;
    return delta;
}

} // namespace

std::shared_ptr<const DecisionTwinTable> make_decision_twin_table(
    const Graph& graph, bool enabled) {
    return std::make_shared<const DecisionTwinTable>(build_twins(graph, enabled));
}

class DecisionSession::Impl {
public:
    struct Frame {
        std::uint32_t cut = 0;
        Graph::Vertex incoming = std::numeric_limits<Graph::Vertex>::max();
        bool entered = false;
        // False only when a sibling below this canonical state was detached.
        // Descendant frames still describe complete canonical subtrees and may
        // publish cache proofs when they fail.
        bool canonical_complete = true;
        bool ownership_claimed = false;
        std::vector<std::uint64_t> ownership_key;
        std::size_t next_candidate = 0;
    };

    struct PendingContinuation {
        std::vector<Graph::Vertex> path;
        std::uint32_t cut = 0;
    };

    Impl(const Graph& graph, std::uint32_t threshold, DecisionOptions options)
        : graph(graph), threshold(threshold), options(std::move(options)), prefix(graph.size()),
          before(graph.size(), 0), bounds(graph, this->options.partial_bounds, threshold),
          twins(this->options.shared_twins ? this->options.shared_twins :
              make_decision_twin_table(graph, this->options.use_twin_symmetry)) {
        // A session owns time externally. A stale per-call limit must not turn a
        // later resume into a timeout.
        this->options.time_limit = std::chrono::milliseconds{0};
        DecisionCacheOptions cache_options{
            this->options.failed_state_cache_limit,
            this->options.failed_state_cache_memory_bytes};
        cache_options.replacement = this->options.cache_replacement;
        cache_options.replacement_page_capacity =
            this->options.cache_replacement_page_capacity;
        const auto words = std::max<std::size_t>(1, graph.word_count());
        shared_fixed_cache = this->options.shared_fixed_cache;
        shared_dynamic_cache = this->options.shared_dynamic_cache;
        bool cache_admitted = true;
        if (!shared_fixed_cache && !shared_dynamic_cache && this->options.memory_governor &&
            this->options.use_failed_state_cache) {
            if (cache_options.max_memory_bytes == 0) {
                cache_admitted = false;
            } else {
                cache_lease = this->options.memory_governor->try_acquire(
                    "threshold-cache-" + std::to_string(threshold),
                    cache_options.max_memory_bytes);
                cache_admitted = cache_lease.has_value();
            }
        }
        if (!cache_admitted) this->options.use_failed_state_cache = false;
        if (this->options.use_failed_state_cache && !shared_fixed_cache &&
            !shared_dynamic_cache) {
            if (this->options.cache_mode == CacheMode::cross_threshold)
                cross_threshold_cache = std::make_unique<DynamicDecisionCache>(words, cache_options);
            else
                fixed_threshold_cache = std::make_unique<FixedThresholdDynamicCache>(
                    words, threshold, cache_options);
        }
        bounds.note_session_ceiling_skips(stats.partial_bounds);
        candidate_arena.resize(graph.size() + 1U);
        rebuild_residual_state();
        rebuild_candidate_index();
        stack.push_back(Frame{});
    }

    Impl(const Graph& graph, const SessionSnapshot& snapshot, DecisionOptions options)
        : Impl(graph, snapshot.threshold, std::move(options)) {
        if (snapshot.status == SessionStatus::cancelled)
            throw std::invalid_argument("cannot resume a cancelled decision session");
        if (snapshot.unfinished_regions == 0 &&
            snapshot.status == SessionStatus::unresolved)
            throw std::invalid_argument("unresolved snapshot has no unfinished region");
        if (snapshot.status == SessionStatus::unresolved && snapshot.frames.empty() &&
            (snapshot.external_regions == 0 || !snapshot.pending.empty()))
            throw std::invalid_argument(
                "unresolved snapshot without a local frame is not waiting solely on children");
        const auto active = snapshot.status == SessionStatus::unresolved &&
            !snapshot.frames.empty() ? 1U : 0U;
        if (snapshot.unfinished_regions != snapshot.pending.size() + active +
                snapshot.external_regions)
            throw std::invalid_argument("snapshot region accounting is inconsistent");
        prefix.clear();
        std::fill(before.begin(), before.end(), 0);
        path.clear();
        for (const auto vertex : snapshot.path) {
            if (vertex >= graph.size() || prefix.contains(vertex))
                throw std::invalid_argument("snapshot has an invalid active path");
            prefix.insert(vertex);
            path.push_back(vertex);
            for (Graph::Vertex other = 0; other < graph.size(); ++other)
                if (!prefix.contains(other) && graph.adjacent(vertex, other)) ++before[other];
        }
        if (!snapshot.frames.empty() && snapshot.frames.size() > path.size() + 1U)
            throw std::invalid_argument("snapshot frame depth exceeds its active path");
        region_root_depth = snapshot.frames.empty()
            ? path.size() : path.size() + 1U - snapshot.frames.size();
        rebuild_residual_state();
        rebuild_candidate_index();
        stack.clear();
        for (std::size_t depth = 0; depth < snapshot.frames.size(); ++depth) {
            const auto& saved = snapshot.frames[depth];
            if (saved.next_candidate > saved.candidates.size())
                throw std::invalid_argument("snapshot frame cursor exceeds candidates");
            Frame frame;
            frame.cut = saved.cut;
            frame.incoming = saved.has_incoming
                ? saved.incoming : std::numeric_limits<Graph::Vertex>::max();
            if (saved.has_incoming && saved.incoming >= graph.size())
                throw std::invalid_argument("snapshot frame has invalid incoming vertex");
            frame.entered = saved.entered;
            // Older snapshots do not identify which ancestors donated. Treat
            // every restored active frame conservatively; newly created child
            // frames regain canonical completeness normally.
            frame.canonical_complete = !snapshot.continuation_partitioned;
            frame.next_candidate = saved.next_candidate;
            auto& candidates = candidate_arena[depth];
            candidates.clear();
            candidates.reserve(saved.candidates.size());
            for (const auto candidate : saved.candidates) {
                if (candidate.vertex >= graph.size())
                    throw std::invalid_argument("snapshot frame has invalid candidate");
                candidates.push_back({candidate.vertex, candidate.cut});
            }
            stack.push_back(std::move(frame));
        }
        pending.clear();
        for (const auto& saved : snapshot.pending) {
            VertexSet seen(graph.size());
            for (const auto vertex : saved.path) {
                if (vertex >= graph.size() || seen.contains(vertex))
                    throw std::invalid_argument("snapshot has an invalid pending path");
                seen.insert(vertex);
            }
            pending.push_back({saved.path, saved.cut});
        }
        solution = snapshot.ordering;
        if (snapshot.status == SessionStatus::feasible &&
            (!graph.validate_ordering(solution) ||
             graph.ordering_cutwidth(solution) > threshold))
            throw std::invalid_argument("snapshot feasible witness is invalid");
        current_status = snapshot.status;
        unfinished_region_count = snapshot.unfinished_regions;
        continuation_partitioned = snapshot.continuation_partitioned;
        local_region_complete = snapshot.status == SessionStatus::unresolved &&
            snapshot.frames.empty();
        stats = snapshot.stats;
        restored_cold_restart_pending = true;
        // A resumed session gets fresh caches. Session-level applicability
        // provenance must not be counted a second time.
    }

    ~Impl() {
        if (!options.canonical_ownership || options.ownership_id == 0) return;
        for (auto& frame : stack) {
            if (frame.ownership_claimed) {
                options.canonical_ownership->abandon(
                    frame.ownership_key, options.ownership_id);
                frame.ownership_claimed = false;
            }
        }
        if (deferred_region_ownership) {
            options.canonical_ownership->abandon(
                deferred_region_ownership_key, options.ownership_id);
            deferred_region_ownership = false;
        }
    }

    SessionSnapshot snapshot(bool allow_external) const {
        const auto active = current_status == SessionStatus::unresolved &&
            !local_region_complete ? 1U : 0U;
        if (unfinished_region_count < pending.size() + active)
            throw std::logic_error("session region accounting underflow in snapshot");
        const auto external = unfinished_region_count - pending.size() - active;
        if (!allow_external && external != 0)
            throw std::logic_error(
                "standalone snapshot cannot omit externally owned continuations");
        SessionSnapshot out;
        out.threshold = threshold;
        out.status = current_status;
        out.path = path;
        out.ordering = solution;
        out.unfinished_regions = unfinished_region_count;
        out.external_regions = external;
        out.continuation_partitioned = continuation_partitioned;
        out.stats = stats;
        out.frames.reserve(stack.size());
        for (std::size_t depth = 0; depth < stack.size(); ++depth) {
            const auto& frame = stack[depth];
            const auto& candidates = candidate_arena[depth];
            SessionFrameSnapshot saved;
            saved.cut = frame.cut;
            saved.has_incoming = frame.incoming !=
                std::numeric_limits<Graph::Vertex>::max();
            saved.incoming = saved.has_incoming ? frame.incoming : 0;
            if (depth == stack.size() - 1 && residual_dp_session.has_value()) {
                saved.entered = false;
            } else {
                saved.entered = frame.entered;
            }
            saved.next_candidate = frame.next_candidate;
            saved.candidates.reserve(candidates.size());
            for (const auto candidate : candidates)
                saved.candidates.push_back({candidate.vertex, candidate.cut});
            out.frames.push_back(std::move(saved));
        }
        out.pending.reserve(pending.size());
        for (const auto& continuation : pending)
            out.pending.push_back({continuation.path, continuation.cut});
        return out;
    }

    bool cache_proves(std::span<const std::uint64_t> key) {
        if (shared_fixed_cache)
            return shared_fixed_cache->proves_failed(key, threshold);
        if (shared_dynamic_cache)
            return shared_dynamic_cache->proves_failed(key, threshold);
        if (!fixed_threshold_cache && !cross_threshold_cache) return false;
        return fixed_threshold_cache
            ? fixed_threshold_cache->proves_failed(key, threshold)
            : cross_threshold_cache->proves_failed(key, threshold);
    }

    bool cache_record(std::span<const std::uint64_t> key) {
        if (shared_fixed_cache)
            return shared_fixed_cache->record_failed(key, threshold);
        if (shared_dynamic_cache)
            return shared_dynamic_cache->record_failed(key, threshold);
        if (!fixed_threshold_cache && !cross_threshold_cache) return false;
        return fixed_threshold_cache
            ? fixed_threshold_cache->record_failed(key, threshold)
            : cross_threshold_cache->record_failed(key, threshold);
    }

    void sync_cache_stats() {
        // Parallel sessions own shared-cache telemetry at the epoch boundary.
        // Scanning every shard here would turn each completed region into an
        // O(shards) locked operation on the DFS return path.
        if (shared_fixed_cache || shared_dynamic_cache) return;
        std::optional<DecisionCacheStats> cache;
        if (fixed_threshold_cache) cache = fixed_threshold_cache->stats();
        else if (cross_threshold_cache) cache = cross_threshold_cache->stats();
        if (!cache) return;
        stats.failed_state_cache_size = cache->entries;
        stats.failed_state_cache_capacity = cache->capacity;
        stats.failed_state_cache_memory_bytes = cache->memory_bytes;
        stats.failed_state_bounds_strengthened = cache->strengthenings;
        stats.failed_state_insertions_skipped = cache->rejected_capacity;
        stats.cache_collisions = cache->collisions;
        stats.cache_segment_growths = cache->segment_growths;
        stats.cache_lookup_probes = cache->lookup_probes;
        stats.cache_insertion_probes = cache->insertion_probes;
        stats.cache_probes_avoided_after_saturation =
            cache->probes_avoided_after_saturation;
        stats.cache_page_promotions = cache->page_promotions;
        stats.cache_page_second_chances = cache->page_second_chances;
        stats.cache_pages_recycled = cache->pages_recycled;
        stats.cache_replacement_admissions = cache->replacement_admissions;
        stats.cache_entries_evicted = cache->entries_evicted;
        stats.cache_evicted_depth_sum = cache->evicted_depth_sum;
        stats.cache_maximum_evicted_depth = cache->maximum_evicted_depth;
    }

    bool twin_blocked(Graph::Vertex v) const {
        for (auto earlier : (*twins)[v]) if (!prefix.contains(earlier)) return true;
        return false;
    }

    bool twin_blocked_after(Graph::Vertex v, Graph::Vertex placed) const {
        for (auto earlier : (*twins)[v])
            if (earlier != placed && !prefix.contains(earlier)) return true;
        return false;
    }

    void safe_point() {
        ++safe_points;
        if (hook) hook(safe_points);
    }

    void apply(Graph::Vertex vertex) {
        if (candidate_index) candidate_index->remove(vertex);
        if (residual_state_enabled) {
            remove_residual_bin(residual_degree[vertex]);
            const auto adjacency = graph.adjacency_words(vertex);
            for (std::size_t wi = 0; wi < adjacency.size(); ++wi) {
                auto word = adjacency[wi];
                while (word) {
                    const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
                    word &= word - 1;
                    const auto other = static_cast<Graph::Vertex>(wi * 64U + bit);
                    if (other < graph.size() && !prefix.contains(other)) {
                        remove_residual_bin(residual_degree[other]);
                        --residual_degree[other];
                        add_residual_bin(residual_degree[other]);
                    }
                }
            }
        }
        prefix.insert(vertex);
        path.push_back(vertex);
        const auto adjacency = graph.adjacency_words(vertex);
        for (std::size_t wi = 0; wi < adjacency.size(); ++wi) {
            auto word = adjacency[wi];
            while (word) {
                const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
                word &= word - 1;
                const auto other = static_cast<Graph::Vertex>(wi * 64U + bit);
                if (other < graph.size() && !prefix.contains(other)) {
                    if (candidate_index) {
                        const auto delta = static_cast<std::int32_t>(graph.degree(other)) -
                            2 * static_cast<std::int32_t>(before[other]);
                        candidate_index->move(other, delta - 2);
                        ++stats.candidate_index_forward_updates;
                    }
                    ++before[other];
                    ++stats.node_state_updates;
                }
            }
        }
        if (options.candidate_enumerator == CandidateEnumerator::cross_check)
            candidate_index->validate(graph, prefix, before);
    }

    void undo(Graph::Vertex vertex) {
        const auto adjacency = graph.adjacency_words(vertex);
        for (std::size_t wi = 0; wi < adjacency.size(); ++wi) {
            auto word = adjacency[wi];
            while (word) {
                const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
                word &= word - 1;
                const auto other = static_cast<Graph::Vertex>(wi * 64U + bit);
                if (other < graph.size() && !prefix.contains(other)) {
                    if (residual_state_enabled) {
                        remove_residual_bin(residual_degree[other]);
                        ++residual_degree[other];
                        add_residual_bin(residual_degree[other]);
                    }
                    --before[other];
                    if (candidate_index) {
                        const auto delta = static_cast<std::int32_t>(graph.degree(other)) -
                            2 * static_cast<std::int32_t>(before[other]);
                        candidate_index->move(other, delta);
                        ++stats.candidate_index_rollback_updates;
                    }
                    ++stats.node_state_updates;
                }
            }
        }
        if (residual_state_enabled) add_residual_bin(residual_degree[vertex]);
        path.pop_back();
        prefix.erase(vertex);
        if (candidate_index) {
            candidate_index->insert(vertex,
                static_cast<std::int32_t>(graph.degree(vertex)) -
                2 * static_cast<std::int32_t>(before[vertex]));
        }
        if (options.candidate_enumerator == CandidateEnumerator::cross_check)
            candidate_index->validate(graph, prefix, before);
    }

    void rebuild_candidate_index() {
        if (options.candidate_enumerator == CandidateEnumerator::scan) {
            candidate_index.reset();
            return;
        }
        candidate_index = std::make_unique<DeltaBucketCandidateIndex>(
            graph, prefix, before);
        if (options.candidate_enumerator == CandidateEnumerator::cross_check)
            candidate_index->validate(graph, prefix, before);
    }

    void rebuild_residual_state() {
        residual_state_enabled = bounds.residual_degree_enabled();
        residual_degree.assign(graph.size(), 0);
        residual_histogram.assign(graph.size() + 1U, 0);
        maximum_residual_degree = 0;
        if (!residual_state_enabled) return;
        for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex) {
            if (prefix.contains(vertex)) continue;
            std::uint32_t degree = 0;
            const auto adjacency = graph.adjacency_words(vertex);
            for (std::size_t word = 0; word < adjacency.size(); ++word)
                degree += static_cast<std::uint32_t>(
                    std::popcount(adjacency[word] & ~prefix.words()[word]));
            residual_degree[vertex] = degree;
            ++residual_histogram[degree];
            maximum_residual_degree = std::max(maximum_residual_degree, degree);
        }
    }

    void remove_residual_bin(std::uint32_t degree) {
        if (degree >= residual_histogram.size() || residual_histogram[degree] == 0)
            throw std::logic_error("residual-degree histogram underflow");
        --residual_histogram[degree];
        ++stats.residual_histogram_updates;
        while (maximum_residual_degree != 0 &&
               residual_histogram[maximum_residual_degree] == 0)
            --maximum_residual_degree;
    }

    void add_residual_bin(std::uint32_t degree) {
        if (degree >= residual_histogram.size())
            throw std::logic_error("residual-degree histogram overflow");
        ++residual_histogram[degree];
        ++stats.residual_histogram_updates;
        maximum_residual_degree = std::max(maximum_residual_degree, degree);
    }

    bool legal_second(Graph::Vertex placed, std::uint32_t cut) {
        ++stats.depth_two_lookahead_checks;
        for (Graph::Vertex v = 0; v < graph.size(); ++v) {
            if (v == placed || prefix.contains(v) || twin_blocked_after(v, placed)) continue;
            const auto count = before[v] + static_cast<std::uint32_t>(graph.adjacent(placed, v));
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(cut) + graph.degree(v) - 2 * count);
            if (next <= threshold) return true;
        }
        return false;
    }

    void enumerate_scan_candidates(std::uint32_t current_cut,
                                   std::vector<Candidate>& candidates,
                                   bool record_stats) {
        if (record_stats) stats.candidate_scan_checks += graph.size() - path.size();
        for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex) {
            if (prefix.contains(vertex)) continue;
            if (twin_blocked(vertex)) {
                if (record_stats) ++stats.twin_symmetric_children_skipped;
                continue;
            }
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(current_cut) + graph.degree(vertex) -
                2 * before[vertex]);
            if (next <= threshold) candidates.push_back({vertex, next});
            else if (record_stats) ++stats.children_rejected_by_cut;
        }
    }

    void enumerate_index_candidates(std::uint32_t current_cut,
                                    std::vector<Candidate>& candidates,
                                    bool record_stats) {
        if (!candidate_index) throw std::logic_error("candidate index is unavailable");
        if (record_stats) ++stats.candidate_index_gathers;
        const auto slots = candidate_index->gather(
            static_cast<std::int32_t>(threshold - current_cut),
            [&](Graph::Vertex vertex, std::int32_t delta) {
                if (record_stats) ++stats.candidate_index_vertices_emitted;
                if (twin_blocked(vertex)) {
                    if (record_stats) ++stats.twin_symmetric_children_skipped;
                    return;
                }
                const auto next = static_cast<std::int64_t>(current_cut) + delta;
                if (next < 0 || next > static_cast<std::int64_t>(threshold))
                    throw std::logic_error("candidate index emitted an illegal vertex");
                candidates.push_back({vertex, static_cast<std::uint32_t>(next)});
            });
        if (record_stats) stats.candidate_index_bucket_slots_visited += slots;
    }

    void enumerate_candidates(std::uint32_t current_cut,
                              std::vector<Candidate>& candidates) {
        if (options.candidate_enumerator == CandidateEnumerator::scan) {
            enumerate_scan_candidates(current_cut, candidates, true);
            return;
        }
        enumerate_index_candidates(current_cut, candidates, true);
        if (options.candidate_enumerator != CandidateEnumerator::cross_check) return;
        std::vector<Candidate> scanned;
        scanned.reserve(graph.size() - path.size());
        enumerate_scan_candidates(current_cut, scanned, false);
        auto indexed = candidates;
        const auto order = [](const Candidate& left, const Candidate& right) {
            if (left.vertex != right.vertex) return left.vertex < right.vertex;
            return left.cut < right.cut;
        };
        std::sort(scanned.begin(), scanned.end(), order);
        std::sort(indexed.begin(), indexed.end(), order);
        if (scanned.size() != indexed.size() ||
            !std::equal(scanned.begin(), scanned.end(), indexed.begin(),
                [](const Candidate& left, const Candidate& right) {
                    return left.vertex == right.vertex && left.cut == right.cut;
                }))
            throw std::logic_error("delta-bucket candidates differ from full scan");
        ++stats.candidate_index_cross_checks;
    }

    bool local_continuation_prunes(std::uint32_t current_cut,
                                   std::size_t viable_children) {
        if (options.local_continuation_depth == 0) return false;
        if (threshold - current_cut > options.local_continuation_max_slack) {
            ++stats.local_continuation_slack_gate_skips;
            return false;
        }
        if (viable_children > options.local_continuation_max_children) {
            ++stats.local_continuation_branch_gate_skips;
            return false;
        }
        ++stats.local_continuation_calls;
        if (!candidate_index)
            throw std::logic_error("local continuation requires a candidate index");
        const auto started = std::chrono::steady_clock::now();
        LocalContinuationProbe probe(
            graph, threshold, prefix, before, *candidate_index, *twins,
            options.candidate_enumerator == CandidateEnumerator::cross_check,
            options.local_continuation_max_states);
        const auto result = probe.run(
            current_cut, options.local_continuation_depth);
        stats.local_continuation_nanoseconds +=
            static_cast<std::uint64_t>(std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count());
        stats.local_continuation_states += result.states;
        stats.local_continuation_cross_checks += result.cross_checks;
        if (!result.conclusive) {
            ++stats.local_continuation_inconclusive;
            return false;
        }
        if (result.continuation_exists) return false;
        ++stats.local_continuation_parent_prunes;
        return true;
    }

    bool record_failure() {
        // Once a continuation donated a descendant, its local search region is
        // no longer the complete canonical prefix. It may contribute region
        // completion, but must not publish a canonical failed-state proof.
        if (stack.back().canonical_complete && options.use_failed_state_cache &&
            cache_record(prefix.words())) {
            ++stats.failed_states_recorded;
            return true;
        }
        return false;
    }

    void record_rejoined_region_failure() {
        // Once every detached child and the donor's local remainder have
        // failed, their union is again the complete canonical region rooted at
        // this prefix. Publish that rejoined proof exactly once.
        if (options.use_failed_state_cache && cache_record(prefix.words()))
            ++stats.failed_states_recorded;
        if (deferred_region_ownership) {
            options.canonical_ownership->publish_failure(
                deferred_region_ownership_key, options.ownership_id);
            deferred_region_ownership = false;
            deferred_region_ownership_key.clear();
        }
    }

    void abandon_all_ownership_claims() noexcept {
        if (!options.canonical_ownership || options.ownership_id == 0) return;
        for (auto& claimed : stack) {
            if (!claimed.ownership_claimed) continue;
            options.canonical_ownership->abandon(
                claimed.ownership_key, options.ownership_id);
            claimed.ownership_claimed = false;
            claimed.ownership_key.clear();
        }
        if (deferred_region_ownership) {
            options.canonical_ownership->abandon(
                deferred_region_ownership_key, options.ownership_id);
            deferred_region_ownership = false;
            deferred_region_ownership_key.clear();
        }
    }

    void load_pending(PendingContinuation pending) {
        prefix.clear();
        std::fill(before.begin(), before.end(), 0);
        path.clear();
        stack.clear();
        for (auto& candidates : candidate_arena) candidates.clear();
        continuation_partitioned = false;
        local_region_complete = false;
        for (const auto vertex : pending.path) {
            prefix.insert(vertex);
            path.push_back(vertex);
            for (Graph::Vertex other = 0; other < graph.size(); ++other)
                if (!prefix.contains(other) && graph.adjacent(vertex, other))
                    ++before[other];
        }
        region_root_depth = path.size();
        rebuild_residual_state();
        rebuild_candidate_index();
        residual_dp_session.reset();
        residual_dp_rejected_this_frame = false;
        Frame root;
        root.cut = pending.cut;
        stack.push_back(std::move(root));
    }

    void complete_region() {
        if (unfinished_region_count == 0)
            throw std::logic_error("session region accounting underflow");
        --unfinished_region_count;
        if (!pending.empty()) {
            auto next = std::move(pending.front());
            pending.pop_front();
            load_pending(std::move(next));
        } else if (unfinished_region_count == 0) {
            current_status = SessionStatus::infeasible;
        } else {
            local_region_complete = true;
        }
    }

    void finish_failed_frame(bool publish = true) {
        auto& frame = stack.back();
        const bool published = publish && record_failure();
        if (frame.ownership_claimed) {
            if (published)
                options.canonical_ownership->publish_failure(
                    frame.ownership_key, options.ownership_id);
            else if (stack.size() == 1 && !frame.canonical_complete &&
                     continuation_partitioned) {
                deferred_region_ownership = true;
                deferred_region_ownership_key = std::move(frame.ownership_key);
            }
            else
                options.canonical_ownership->abandon(
                    frame.ownership_key, options.ownership_id);
            frame.ownership_claimed = false;
        }
        const auto incoming = frame.incoming;
        candidate_arena[stack.size() - 1U].clear();
        stack.pop_back();
        if (incoming != std::numeric_limits<Graph::Vertex>::max()) undo(incoming);
        if (stack.empty()) complete_region();
        residual_dp_session.reset();
        residual_dp_rejected_this_frame = false;
    }

    bool donate_unexplored_sibling() {
        if (current_status != SessionStatus::unresolved || stack.empty()) return false;
        for (std::size_t depth = stack.size(); depth-- > 0;) {
            auto& frame = stack[depth];
            auto& candidates = candidate_arena[depth];
            if (!frame.entered || frame.next_candidate >= candidates.size()) continue;
            const auto candidate = candidates[frame.next_candidate++];
            PendingContinuation donated;
            const auto donated_parent_depth = region_root_depth + depth;
            if (donated_parent_depth > path.size())
                throw std::logic_error("session root depth exceeds active donation path");
            donated.path.assign(path.begin(),
                path.begin() + static_cast<std::ptrdiff_t>(donated_parent_depth));
            donated.path.push_back(candidate.vertex);
            donated.cut = candidate.cut;
            // Reserve and construct before publishing or changing proof
            // accounting. Allocation failure therefore leaves the donor exact.
            pending.push_back(std::move(donated));
            ++unfinished_region_count;
            for (std::size_t ancestor = 0; ancestor <= depth; ++ancestor)
                stack[ancestor].canonical_complete = false;
            continuation_partitioned = true;
            return true;
        }
        return false;
    }

    std::optional<SessionSnapshot> extract_pending_continuation() {
        if (current_status != SessionStatus::unresolved || pending.empty())
            return std::nullopt;
        auto continuation = std::move(pending.front());
        pending.pop_front();
        SessionSnapshot child;
        child.threshold = threshold;
        child.status = SessionStatus::unresolved;
        child.path = std::move(continuation.path);
        child.unfinished_regions = 1;
        SessionFrameSnapshot root;
        root.cut = continuation.cut;
        child.frames.push_back(std::move(root));
        return child;
    }

    void resolve_external_failure() {
        if (current_status != SessionStatus::unresolved || unfinished_region_count == 0)
            throw std::logic_error("external failure has no matching region");
        --unfinished_region_count;
        if (unfinished_region_count == 0) {
            if (!local_region_complete || !pending.empty())
                throw std::logic_error("external failure closed a live local region");
            record_rejoined_region_failure();
            current_status = SessionStatus::infeasible;
        }
    }

    void mark_certified_infeasible() {
        if (current_status != SessionStatus::unresolved)
            throw std::logic_error("certified bound closed a terminal region");
        const auto active = local_region_complete ? 0U : 1U;
        if (unfinished_region_count < pending.size() + active)
            throw std::logic_error("certified bound found inconsistent region accounting");
        const auto external = unfinished_region_count - pending.size() - active;
        if (unfinished_region_count != 1 || external != 0 ||
            !pending.empty())
            throw std::logic_error("certified bound may retire only a whole unstarted region");
        current_status = SessionStatus::infeasible;
        stack.clear();
        candidate_arena.clear();
    }

    void publish_external_witness(const std::vector<Graph::Vertex>& witness) {
        if (current_status != SessionStatus::unresolved) return;
        if (!graph.validate_ordering(witness) || graph.ordering_cutwidth(witness) > threshold)
            throw std::invalid_argument("external session witness is invalid");
        solution = witness;
        current_status = SessionStatus::feasible;
    }

    SessionServiceEvent service(const SessionServiceBudget& budget) {
        const auto before_stats = stats;
        const auto before_safe = safe_points;
        SessionYieldReason reason = SessionYieldReason::quantum_complete;
        if (current_status != SessionStatus::unresolved)
            return event(before_stats, before_safe, SessionYieldReason::terminal);
        if (local_region_complete)
            return event(before_stats, before_safe, SessionYieldReason::worker_donation);
        yield_requested.store(false, std::memory_order_relaxed);
        const auto target = budget.work_units == 0 ? 1 : budget.work_units;
        std::uint64_t expanded = 0;
        try {
            while (current_status == SessionStatus::unresolved) {
                if (local_region_complete) {
                    reason = SessionYieldReason::worker_donation;
                    break;
                }
                if (residual_dp_session.has_value()) {
                    const std::uint64_t remaining_work = target > expanded ? (target - expanded) : 1;
                    const auto dp_started = std::chrono::steady_clock::now();
                    auto event = residual_dp_session->service(remaining_work, budget.absolute_deadline);
                    const auto dp_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - dp_started).count();
                    stats.residual_dp_seconds += dp_elapsed;
                    stats.residual_dp_states += event.states_completed;
                    expanded += event.states_completed;
                    if (!event.complete) {
                        if (cancelled.load(std::memory_order_relaxed)) {
                            current_status = SessionStatus::cancelled;
                            reason = SessionYieldReason::interval_resolved;
                        } else if (std::chrono::steady_clock::now() >= budget.absolute_deadline) {
                            reason = SessionYieldReason::deadline;
                        } else if (yield_requested.load(std::memory_order_relaxed)) {
                            reason = SessionYieldReason::yield_requested;
                        } else {
                            reason = SessionYieldReason::quantum_complete;
                        }
                        break;
                    }
                    if (*event.exact_completion > threshold) {
                        residual_dp_session.reset();
                        residual_dp_rejected_this_frame = false;
                        ++stats.residual_dp_completed_tails;
                        ++stats.residual_dp_infeasible_prunes;
                        finish_failed_frame(true);
                    } else {
                        auto remaining_ordering = residual_dp_session->reconstruct_witness();
                        std::vector<Graph::Vertex> full_ordering = path;
                        full_ordering.insert(full_ordering.end(), remaining_ordering.begin(), remaining_ordering.end());
                        if (!graph.validate_ordering(full_ordering) || graph.ordering_cutwidth(full_ordering) > threshold) {
                            throw std::logic_error("residual DP reconstructed an invalid witness");
                        }
                        solution = full_ordering;
                        current_status = SessionStatus::feasible;
                        reason = SessionYieldReason::terminal;
                        residual_dp_session.reset();
                        residual_dp_rejected_this_frame = false;
                        ++stats.residual_dp_completed_tails;
                        ++stats.residual_dp_feasible_witnesses;
                    }
                    if (expanded >= target) break;
                    continue;
                }
                safe_point();
                if (cancelled.load(std::memory_order_relaxed)) {
                    current_status = SessionStatus::cancelled;
                    reason = SessionYieldReason::interval_resolved;
                    break;
                }
                if (std::chrono::steady_clock::now() >= budget.absolute_deadline) {
                    reason = SessionYieldReason::deadline;
                    break;
                }
                if (yield_requested.load(std::memory_order_relaxed)) {
                    reason = SessionYieldReason::yield_requested;
                    break;
                }
                auto& frame = stack.back();
                if (!frame.entered) {
                    if (path.size() == graph.size()) {
                        frame.entered = true;
                        ++stats.nodes_expanded;
                        const auto residual = graph.size() - path.size();
                        if (static_cast<std::uint32_t>(residual) < stats.dfs_min_remaining_vertices) {
                            stats.dfs_min_remaining_vertices = static_cast<std::uint32_t>(residual);
                        }
                        ++expanded;
                        solution = path;
                        if (!graph.validate_ordering(solution) ||
                            graph.ordering_cutwidth(solution) > threshold)
                            throw std::logic_error("session produced an invalid witness");
                        current_status = SessionStatus::feasible;
                        reason = SessionYieldReason::terminal;
                        break;
                    }
                    if (options.use_failed_state_cache) {
                        ++stats.failed_cache_queries;
                        if (cache_proves(prefix.words())) {
                            ++stats.failed_cache_hits;
                            // The shared cache already owns this exact proof.
                            // Re-inserting it would add a second locked probe to
                            // the hottest successful-lookup path.
                            finish_failed_frame(false);
                            if (expanded >= target) break;
                            continue;
                        }
                    }
                    // Ownership coordinates donated region roots only. Taking
                    // claims at every recursive frame creates needless claim
                    // churn and can serialize ordinary in-session DFS.
                    if (stack.size() == 1 && options.canonical_ownership &&
                        options.ownership_id != 0) {
                        const auto acquired = options.canonical_ownership->acquire(
                            prefix.words(), options.ownership_id);
                        if (acquired == OwnershipAcquire::duplicate) {
                            ++stats.duplicate_ownership_waits;
                            // Release every older claim before parking. Claims
                            // therefore cannot form a wait cycle across DFS
                            // continuations. A wakeup always retries the cache
                            // and never treats ownership itself as a proof.
                            abandon_all_ownership_claims();
                            reason = SessionYieldReason::ownership_wait;
                            break;
                        }
                        else if (acquired == OwnershipAcquire::saturated) {
                            ++stats.ownership_saturation;
                        } else {
                            ++stats.unique_canonical_claims;
                            frame.ownership_claimed = true;
                            frame.ownership_key.assign(
                                prefix.words().begin(), prefix.words().end());
                        }
                    }
                    frame.entered = true;
                    ++stats.nodes_expanded;
                    const auto residual = graph.size() - path.size();
                    if (static_cast<std::uint32_t>(residual) < stats.dfs_min_remaining_vertices) {
                        stats.dfs_min_remaining_vertices = static_cast<std::uint32_t>(residual);
                    }
                    ++expanded;
                    if (options.use_partial_bounds && bounds.completion_exceeds(
                            prefix.words(), static_cast<std::uint32_t>(path.size()),
                            threshold, stats.partial_bounds,
                            residual_state_enabled
                                ? std::optional<std::uint32_t>{maximum_residual_degree}
                                : std::nullopt,
                            frame.cut)) {
                        finish_failed_frame();
                        if (expanded >= target) break;
                        continue;
                    }
                    const auto remaining = graph.size() - path.size();
                    if (remaining >= 1 && remaining <= options.dfs_residual_dp_max_remaining && !residual_dp_rejected_this_frame) {
                        if (restored_cold_restart_pending) {
                            ++stats.residual_dp_cold_restarts;
                            restored_cold_restart_pending = false;
                        }
                        ++stats.residual_dp_attempts;
                        auto proj = project_residual_dp(remaining, graph.word_count());
                        if (proj.has_value() && proj->peak_bytes <= options.residual_dp_max_bytes && options.memory_governor) {
                            residual_dp_session.emplace(graph, prefix.words(), options.memory_governor);
                            if (residual_dp_session->applicable()) {
                                ++stats.residual_dp_admissions;
                                stats.residual_dp_peak_bytes = std::max(stats.residual_dp_peak_bytes, static_cast<std::uint64_t>(proj->peak_bytes));
                                continue;
                            } else {
                                residual_dp_session.reset();
                                residual_dp_rejected_this_frame = true;
                                ++stats.residual_dp_governor_or_cap_rejections;
                            }
                        } else {
                            residual_dp_rejected_this_frame = true;
                            ++stats.residual_dp_governor_or_cap_rejections;
                        }
                    }
                    auto& candidates = candidate_arena[stack.size() - 1U];
                    candidates.clear();
                    candidates.reserve(graph.size() - path.size());
                    enumerate_candidates(frame.cut, candidates);
                    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
                        if (a.cut != b.cut)
                            return options.use_fail_first_candidate_order ? a.cut > b.cut : a.cut < b.cut;
                        return a.vertex < b.vertex;
                    });
                    if (local_continuation_prunes(frame.cut, candidates.size())) {
                        finish_failed_frame();
                        if (expanded >= target) break;
                        continue;
                    }
                    safe_point();
                }

                auto& active = stack.back();
                auto& candidates = candidate_arena[stack.size() - 1U];
                bool descended = false;
                while (active.next_candidate < candidates.size()) {
                    const auto candidate = candidates[active.next_candidate++];
                    const auto remaining = graph.size() - path.size() - 1;
                    const bool gate = options.depth_two_lookahead_max_remaining == 0 ||
                                      remaining <= options.depth_two_lookahead_max_remaining;
                    if (options.use_depth_two_lookahead && gate && path.size() + 1 < graph.size() &&
                        !legal_second(candidate.vertex, candidate.cut)) {
                        ++stats.children_rejected_by_depth_two_lookahead;
                        continue;
                    }
                    apply(candidate.vertex);
                    Frame child;
                    child.cut = candidate.cut;
                    child.incoming = candidate.vertex;
                    stack.push_back(std::move(child));
                    descended = true;
                    residual_dp_session.reset();
                    residual_dp_rejected_this_frame = false;
                    break;
                }
                if (!descended) finish_failed_frame();
                if (expanded >= target) break;
            }
        } catch (...) {
            reason = SessionYieldReason::exception;
            throw;
        }
        if (current_status != SessionStatus::unresolved) reason = SessionYieldReason::terminal;
        sync_cache_stats();
        return event(before_stats, before_safe, reason);
    }

    SessionServiceEvent event(const DecisionStats& before_stats, std::uint64_t before_safe,
                              SessionYieldReason reason) const {
        SessionServiceEvent result;
        result.threshold = threshold;
        result.reason = reason;
        result.status = current_status;
        result.delta = subtract_stats(stats, before_stats);
        result.delta.failed_state_cache_size = stats.failed_state_cache_size;
        result.delta.failed_state_cache_capacity = stats.failed_state_cache_capacity;
        result.delta.failed_state_cache_memory_bytes = stats.failed_state_cache_memory_bytes;
        result.safe_points = safe_points - before_safe;
        result.right_censored = current_status == SessionStatus::unresolved;
        return result;
    }

    const Graph& graph;
    std::uint32_t threshold;
    DecisionOptions options;
    VertexSet prefix;
    std::vector<std::uint32_t> before;
    std::vector<std::uint32_t> residual_degree;
    std::vector<std::uint32_t> residual_histogram;
    std::uint32_t maximum_residual_degree = 0;
    bool residual_state_enabled = false;
    std::vector<Graph::Vertex> path;
    // Count of inherited prefix vertices before this session's root frame.
    // Frame depth is relative to this boundary after restore and donation.
    std::size_t region_root_depth = 0;
    std::vector<Graph::Vertex> solution;
    std::vector<Frame> stack;
    // One reusable candidate vector per DFS depth. This keeps resumability
    // without allocating and freeing a vector at every expanded state.
    std::vector<std::vector<Candidate>> candidate_arena;
    std::unique_ptr<DeltaBucketCandidateIndex> candidate_index;
    PartialBoundEvaluator bounds;
    std::unique_ptr<DynamicDecisionCache> cross_threshold_cache;
    std::unique_ptr<FixedThresholdDynamicCache> fixed_threshold_cache;
    std::optional<MemoryGovernor::Lease> cache_lease;
    std::shared_ptr<ShardedFixedThresholdDynamicCache> shared_fixed_cache;
    std::shared_ptr<ShardedDynamicDecisionCache> shared_dynamic_cache;
    std::shared_ptr<const TwinTable> twins;
    DecisionStats stats;
    SessionStatus current_status = SessionStatus::unresolved;
    std::atomic<bool> yield_requested{false};
    std::atomic<bool> cancelled{false};
    std::uint64_t safe_points = 0;
    SafePointHook hook;
    std::deque<PendingContinuation> pending;
    std::uint64_t unfinished_region_count = 1;
    bool continuation_partitioned = false;
    bool deferred_region_ownership = false;
    std::vector<std::uint64_t> deferred_region_ownership_key;
    bool local_region_complete = false;
    // Persistent per-tail residual DP session. Active only for the current
    // unentered tail when admitted; destroyed on frame entry/pop.
    std::optional<ResidualDpSession> residual_dp_session;
    // Set when the governor or byte-cap rejected residual DP for the current
    // tail. Prevents repeated admission attempts for the same frame.
    bool residual_dp_rejected_this_frame = false;
    bool restored_cold_restart_pending = false;
};

DecisionSession::DecisionSession(const Graph& graph, std::uint32_t threshold,
                                 DecisionOptions options)
    : impl_(std::make_unique<Impl>(graph, threshold, std::move(options))) {}
DecisionSession::DecisionSession(const Graph& graph, const SessionSnapshot& snapshot,
                                 DecisionOptions options)
    : impl_(std::make_unique<Impl>(graph, snapshot, std::move(options))) {}
DecisionSession::~DecisionSession() = default;
DecisionSession::DecisionSession(DecisionSession&&) noexcept = default;
DecisionSession& DecisionSession::operator=(DecisionSession&&) noexcept = default;
SessionServiceEvent DecisionSession::service(const SessionServiceBudget& budget) { return impl_->service(budget); }
bool DecisionSession::donate_unexplored_sibling() { return impl_->donate_unexplored_sibling(); }
std::optional<SessionSnapshot> DecisionSession::extract_pending_continuation() {
    return impl_->extract_pending_continuation();
}
void DecisionSession::resolve_external_failure() { impl_->resolve_external_failure(); }
void DecisionSession::mark_certified_infeasible() { impl_->mark_certified_infeasible(); }
void DecisionSession::publish_external_witness(
    const std::vector<Graph::Vertex>& ordering) {
    impl_->publish_external_witness(ordering);
}
void DecisionSession::request_yield() noexcept { impl_->yield_requested.store(true, std::memory_order_relaxed); }
void DecisionSession::cancel() noexcept {
    impl_->cancelled.store(true, std::memory_order_relaxed);
    impl_->residual_dp_session.reset();
    impl_->residual_dp_rejected_this_frame = false;
}
SessionStatus DecisionSession::status() const noexcept { return impl_->current_status; }
std::uint32_t DecisionSession::threshold() const noexcept { return impl_->threshold; }
const std::vector<Graph::Vertex>& DecisionSession::ordering() const noexcept { return impl_->solution; }
const DecisionStats& DecisionSession::stats() const noexcept { return impl_->stats; }
std::size_t DecisionSession::stack_depth() const noexcept { return impl_->stack.size(); }
std::uint64_t DecisionSession::unfinished_regions() const noexcept { return impl_->unfinished_region_count; }
std::size_t DecisionSession::pending_continuations() const noexcept { return impl_->pending.size(); }
SessionSnapshot DecisionSession::quiesce_and_snapshot() const { return impl_->snapshot(false); }
SessionSnapshot DecisionSession::quiesce_region_snapshot() const { return impl_->snapshot(true); }
bool DecisionSession::waiting_for_external_regions() const noexcept {
    return impl_->local_region_complete;
}
void DecisionSession::set_safe_point_hook(SafePointHook hook) { impl_->hook = std::move(hook); }

} // namespace cutwidth
