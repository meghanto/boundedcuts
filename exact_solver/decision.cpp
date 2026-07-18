#include "decision.hpp"

#include "delta_bucket_candidate_index.hpp"
#include "local_continuation_bound.hpp"
#include "vertex_set.hpp"
#include "residual_dp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

namespace cutwidth {

void merge_dfs_diagnostics(DfsDiagnostics& total, const DfsDiagnostics& part) {
    if (!part.enabled) return;
    if (!total.enabled) {
        total = part;
        return;
    }
    if (total.threshold != part.threshold)
        throw std::logic_error("cannot merge DFS diagnostics from different thresholds");
    total.nodes_entered += part.nodes_entered;
    auto merge_bound = [](DfsBoundDiagnostics& into,
                          const DfsBoundDiagnostics& from) {
        into.evaluations += from.evaluations;
        into.prunes += from.prunes;
        into.nanoseconds += from.nanoseconds;
    };
    merge_bound(total.sdp_bound, part.sdp_bound);
    merge_bound(total.partial_bound, part.partial_bound);
    merge_bound(total.residual_dp, part.residual_dp);
    merge_bound(total.best_next_bound, part.best_next_bound);
    if (total.by_depth.size() < part.by_depth.size())
        total.by_depth.resize(part.by_depth.size());
    for (std::size_t depth = 0; depth < part.by_depth.size(); ++depth) {
        auto& into = total.by_depth[depth];
        const auto& from = part.by_depth[depth];
        into.nodes_entered += from.nodes_entered;
        into.cache_queries += from.cache_queries;
        into.cache_hits += from.cache_hits;
        into.cache_prunes += from.cache_prunes;
        into.sdp_prunes += from.sdp_prunes;
        into.partial_bound_prunes += from.partial_bound_prunes;
        into.residual_dp_prunes += from.residual_dp_prunes;
        into.best_next_prunes += from.best_next_prunes;
        into.dead_ends += from.dead_ends;
        into.children_rejected_by_cut += from.children_rejected_by_cut;
        into.children_rejected_by_symmetry += from.children_rejected_by_symmetry;
        into.children_rejected_by_lookahead += from.children_rejected_by_lookahead;
        into.viable_child_observations += from.viable_child_observations;
        into.viable_children_sum += from.viable_children_sum;
        for (std::size_t i = 0; i < into.viable_child_histogram.size(); ++i)
            into.viable_child_histogram[i] += from.viable_child_histogram[i];
        for (std::size_t i = 0; i < into.slack_histogram.size(); ++i)
            into.slack_histogram[i] += from.slack_histogram[i];
    }
    for (const auto& from : part.by_root) {
        auto found = std::find_if(total.by_root.begin(), total.by_root.end(),
            [&](const DfsRootDiagnostics& entry) {
                return entry.root_vertex == from.root_vertex;
            });
        if (found == total.by_root.end()) {
            total.by_root.push_back(from);
            continue;
        }
        found->nodes_entered += from.nodes_entered;
        found->total_prunes += from.total_prunes;
        found->cache_prunes += from.cache_prunes;
        found->bound_prunes += from.bound_prunes;
        found->dead_ends += from.dead_ends;
        found->maximum_depth = std::max(found->maximum_depth, from.maximum_depth);
        if (found->nodes_by_depth.size() < from.nodes_by_depth.size())
            found->nodes_by_depth.resize(from.nodes_by_depth.size());
        for (std::size_t depth = 0; depth < from.nodes_by_depth.size(); ++depth)
            found->nodes_by_depth[depth] += from.nodes_by_depth[depth];
    }
}

namespace {

using Clock = std::chrono::steady_clock;
enum class Outcome { found, failed, timed_out };
enum class LookaheadOutcome { legal_second, no_legal_second, timed_out };

[[nodiscard]] std::size_t viable_child_bucket(std::size_t count) noexcept {
    if (count <= 4) return count;
    if (count <= 7) return 5;
    if (count <= 15) return 6;
    if (count <= 31) return 7;
    if (count <= 63) return 8;
    return 9;
}

[[nodiscard]] std::size_t slack_bucket(std::uint32_t slack) noexcept {
    if (slack <= 4) return slack;
    if (slack <= 7) return 5;
    if (slack <= 15) return 6;
    return 7;
}

class ReversibleNodeState64 {
public:
    ReversibleNodeState64(const Graph& graph, Graph::Mask prefix, std::uint32_t cut,
                          DecisionStats& stats)
        : graph_(graph), prefix_(prefix), cut_(cut), before_(graph.size()),
          residual_(graph.size()), histogram_(graph.size() + 1),
          gain_buckets_(2 * graph.size() + 1), stats_(stats) {
        const auto all = graph.size() == 0 ? Graph::Mask{0} :
            (Graph::Mask{1} << graph.size()) - 1;
        const auto remaining = all & ~prefix;
        for (Graph::Vertex v = 0; v < graph.size(); ++v) {
            before_[v] = static_cast<std::uint32_t>(std::popcount(graph.adjacency(v) & prefix));
            residual_[v] = static_cast<std::uint32_t>(std::popcount(graph.adjacency(v) & remaining));
            if ((remaining & (Graph::Mask{1} << v)) != 0) {
                ++histogram_[residual_[v]]; add_gain(v);
            }
        }
    }
    [[nodiscard]] std::uint32_t before(Graph::Vertex v) const { return before_[v]; }
    [[nodiscard]] std::uint32_t residual(Graph::Vertex v) const { return residual_[v]; }
    [[nodiscard]] std::uint32_t cut() const { return cut_; }
    [[nodiscard]] Graph::Mask gain_bucket(std::size_t index) const {
        return gain_buckets_[index];
    }
    [[nodiscard]] std::size_t gain_bucket_count() const { return gain_buckets_.size(); }
    [[nodiscard]] std::uint32_t maximum_residual_degree() const {
        for (std::size_t d = histogram_.size(); d-- > 0;)
            if (histogram_[d] != 0) return static_cast<std::uint32_t>(d);
        return 0;
    }
    void push(Graph::Vertex vertex, std::uint32_t next_cut) {
        const auto bit = Graph::Mask{1} << vertex;
        remove_gain(vertex);
        --histogram_[residual_[vertex]];
        auto neighbors = graph_.adjacency(vertex) & ~prefix_ & ~bit;
        while (neighbors) {
            const auto other = static_cast<Graph::Vertex>(std::countr_zero(neighbors));
            neighbors &= neighbors - 1;
            remove_gain(other);
            --histogram_[residual_[other]];
            --residual_[other]; ++before_[other];
            ++histogram_[residual_[other]];
            add_gain(other);
            ++stats_.node_state_updates;
        }
        prefix_ |= bit; cut_ = next_cut; ++stats_.node_state_updates;
    }
    void pop(Graph::Vertex vertex, std::uint32_t previous_cut) {
        const auto bit = Graph::Mask{1} << vertex;
        prefix_ &= ~bit;
        auto neighbors = graph_.adjacency(vertex) & ~prefix_;
        while (neighbors) {
            const auto other = static_cast<Graph::Vertex>(std::countr_zero(neighbors));
            neighbors &= neighbors - 1;
            remove_gain(other);
            --histogram_[residual_[other]];
            ++residual_[other]; --before_[other];
            ++histogram_[residual_[other]];
            add_gain(other);
            ++stats_.node_state_updates;
        }
        ++histogram_[residual_[vertex]];
        add_gain(vertex);
        cut_ = previous_cut; ++stats_.node_state_updates;
    }
private:
    [[nodiscard]] std::size_t gain_index(Graph::Vertex v) const {
        const auto gain = static_cast<std::int64_t>(residual_[v]) - before_[v];
        return static_cast<std::size_t>(gain + static_cast<std::int64_t>(graph_.size()));
    }
    void add_gain(Graph::Vertex v) { gain_buckets_[gain_index(v)] |= Graph::Mask{1} << v; }
    void remove_gain(Graph::Vertex v) { gain_buckets_[gain_index(v)] &= ~(Graph::Mask{1} << v); }
    const Graph& graph_; Graph::Mask prefix_; std::uint32_t cut_;
    std::vector<std::uint32_t> before_, residual_, histogram_;
    std::vector<Graph::Mask> gain_buckets_; DecisionStats& stats_;
};

struct NodeStateRollback {
    ReversibleNodeState64* state;
    Graph::Vertex vertex;
    std::uint32_t previous_cut;
    ~NodeStateRollback() { if (state) state->pop(vertex, previous_cut); }
};

DecisionCacheOptions cache_options(const DecisionOptions& options) {
    DecisionCacheOptions result{
        options.failed_state_cache_limit, options.failed_state_cache_memory_bytes};
    result.replacement = options.cache_replacement;
    result.replacement_page_capacity = options.cache_replacement_page_capacity;
    return result;
}

void copy_cache_delta(DecisionStats& out, const DecisionCacheStats& before,
                      const DecisionCacheStats& after) {
    out.failed_cache_hits = after.hits - before.hits;
    out.failed_states_recorded = after.inserts - before.inserts;
    out.failed_state_bounds_strengthened = after.strengthenings - before.strengthenings;
    out.failed_state_insertions_skipped = after.rejected_capacity - before.rejected_capacity;
    out.cache_collisions = after.collisions - before.collisions;
    out.failed_state_cache_size = after.entries;
    out.failed_state_cache_capacity = after.capacity;
    out.failed_state_cache_memory_bytes = after.memory_bytes;
    out.cache_page_promotions = after.page_promotions - before.page_promotions;
    out.cache_page_second_chances =
        after.page_second_chances - before.page_second_chances;
    out.cache_pages_recycled = after.pages_recycled - before.pages_recycled;
    out.cache_replacement_admissions =
        after.replacement_admissions - before.replacement_admissions;
    out.cache_entries_evicted = after.entries_evicted - before.entries_evicted;
    out.cache_evicted_depth_sum = after.evicted_depth_sum - before.evicted_depth_sum;
    out.cache_maximum_evicted_depth = after.maximum_evicted_depth;
}

template <typename Cache>
class Search64 {
public:
    Search64(const Graph& graph, std::uint32_t threshold, DecisionOptions options,
             Cache& cache, Graph::Mask initial_prefix = 0,
             std::uint32_t initial_cut = 0,
             std::vector<Graph::Vertex> initial_path = {},
             const std::atomic<bool>* external_stop = nullptr,
             bool collect_cache_delta = true)
        : graph_(graph), threshold_(threshold), options_(options), cache_(cache),
          all_(graph.size() == 0 ? 0 : (Graph::Mask{1} << graph.size()) - 1),
          initial_prefix_(initial_prefix), initial_cut_(initial_cut),
          bounds_(graph, options.partial_bounds, threshold),
          cache_before_(collect_cache_delta ? cache.stats() : DecisionCacheStats{}),
          external_stop_(external_stop), collect_cache_delta_(collect_cache_delta),
          path_(std::move(initial_path)) {
        if (!graph.supports_mask()) throw std::invalid_argument("64-bit decision backend requires at most 63 vertices");
        twins_.assign(graph.size(), 0);
        if (options_.use_twin_symmetry) {
            for (Graph::Vertex later = 0; later < graph.size(); ++later) {
                for (Graph::Vertex earlier = 0; earlier < later; ++earlier) {
                    const auto eb = Graph::Mask{1} << earlier;
                    const auto lb = Graph::Mask{1} << later;
                    if (graph.adjacency(earlier) == graph.adjacency(later) ||
                        (graph.adjacency(earlier) | eb) == (graph.adjacency(later) | lb))
                        twins_[later] |= eb;
                }
            }
        }
        if (options_.time_limit.count() > 0) deadline_ = Clock::now() + options_.time_limit;
        if (options_.node_memo_depth > 4) throw std::invalid_argument("node memo depth must be between 0 and 4");
        if (options_.node_memo_depth != 0) {
            oracle_ = std::make_unique<FiniteHorizonOracle>(graph_, options_.node_memo);
            stats_.node_memo_available = true;
        }
        if (options_.node_memo) memo_before_ = options_.node_memo->stats();
        if (options_.node_state == NodeStateMode::incremental)
            state_ = std::make_unique<ReversibleNodeState64>(graph_, initial_prefix_, initial_cut_, stats_);
    }

    DecisionResult run() {
        path_.reserve(graph_.size());
        const auto outcome = dfs(initial_prefix_, initial_cut_);
        DecisionResult result;
        result.threshold = threshold_;
        result.status = outcome == Outcome::found ? DecisionStatus::feasible :
                        outcome == Outcome::failed ? DecisionStatus::infeasible : DecisionStatus::timed_out;
        if (outcome == Outcome::found) result.ordering = solution_;
        result.stats = stats_;
        if (options_.node_memo) {
            const auto memo = options_.node_memo->stats();
            for (std::size_t d = 0; d < memo.hits_by_depth.size(); ++d)
                result.stats.node_memo_hits_by_depth[d] =
                    memo.hits_by_depth[d] - memo_before_.hits_by_depth[d];
            result.stats.node_memo_computations = memo.computations - memo_before_.computations;
            result.stats.node_memo_collisions = memo.collisions - memo_before_.collisions;
            result.stats.node_memo_saturation = memo.saturation - memo_before_.saturation;
            result.stats.node_memo_memory_bytes = memo.memory_bytes;
        }
        if (collect_cache_delta_) copy_cache_delta(result.stats, cache_before_, cache_.stats());
        return result;
    }

private:
    bool expired() const {
        return (external_stop_ != nullptr && external_stop_->load(std::memory_order_relaxed)) ||
               (options_.time_limit.count() > 0 && Clock::now() >= deadline_);
    }
    void record(Graph::Mask prefix) {
        if (options_.use_failed_state_cache) (void)cache_.record_failed(prefix, threshold_);
    }
    void record_through(Graph::Mask prefix, std::uint32_t maximum_failed) {
        if (options_.use_failed_state_cache &&
            !cache_.record_failed(prefix, maximum_failed) && maximum_failed != threshold_)
            (void)cache_.record_failed(prefix, threshold_);
    }
    LookaheadOutcome has_legal_second(Graph::Mask prefix, std::uint32_t current_cut) {
        ++stats_.depth_two_lookahead_checks;
        const auto unplaced = all_ & ~prefix;
        auto scan = unplaced;
        while (scan != 0) {
            if (expired()) return LookaheadOutcome::timed_out;
            const auto v = static_cast<Graph::Vertex>(std::countr_zero(scan));
            scan &= scan - 1;
            if ((twins_[v] & unplaced) != 0) continue;
            const auto before = static_cast<std::uint32_t>(
                std::popcount(graph_.adjacency(v) & prefix));
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(current_cut) + graph_.degree(v) - 2 * before);
            if (next <= threshold_) return LookaheadOutcome::legal_second;
        }
        return LookaheadOutcome::no_legal_second;
    }
    Outcome dfs(Graph::Mask prefix, std::uint32_t current_cut) {
        if (expired()) return Outcome::timed_out;
        ++stats_.nodes_expanded;
        if (prefix == all_) { solution_ = path_; return Outcome::found; }
        if (options_.use_failed_state_cache) {
            ++stats_.failed_cache_queries;
            if (cache_.proves_failed(prefix, threshold_)) return Outcome::failed;
        }
        const auto residual = static_cast<std::size_t>(std::popcount(all_ & ~prefix));
        if (static_cast<std::uint32_t>(residual) < stats_.dfs_min_remaining_vertices) {
            stats_.dfs_min_remaining_vertices = static_cast<std::uint32_t>(residual);
        }
        const bool memo_gate = oracle_ && (options_.node_memo_max_remaining == 0 ||
            residual <= options_.node_memo_max_remaining);
        if (memo_gate) {
            const auto bound = oracle_->evaluate(prefix, options_.node_memo_depth,
                                                  deadline_, external_stop_);
            if (bound.complete && bound.bound > threshold_) {
                ++stats_.node_memo_prunes;
                record_through(prefix, bound.bound - 1);
                return Outcome::failed;
            }
        }
        if (options_.sdp_oracle && stats_.nodes_expanded >= next_sdp_node_ &&
            residual > 1 && options_.sdp_oracle->should_attempt(
                residual + 1, stats_.nodes_expanded)) {
                ++stats_.sdp_requests;
                next_sdp_node_ = stats_.nodes_expanded +
                    std::max<std::uint64_t>(1, options_.sdp_oracle->trigger_nodes());
                const Graph::Mask word = prefix;
                sdp::SdpBoundRequest request;
                request.prefix = std::span<const Graph::Mask>(&word, 1);
                request.cardinality = residual / 2;
                request.accumulated_subtree_nodes = stats_.nodes_expanded;
                request.existing_certified_bound = threshold_;
                request.caller_deadline = deadline_;
                const auto bound = options_.sdp_oracle->bound(request);
                if (bound.certified_lower_bound) {
                    ++stats_.sdp_certified;
                    if (*bound.certified_lower_bound > threshold_) {
                        ++stats_.sdp_prunes;
                        record(prefix);
                        return Outcome::failed;
                    }
                }
        }
        if (options_.use_partial_bounds && options_.node_state == NodeStateMode::incremental &&
            options_.partial_bounds.residual_degree && !options_.partial_bounds.edge_distance_area &&
            !options_.partial_bounds.degree_distance_area && !options_.partial_bounds.degeneracy) {
            ++stats_.partial_bounds.evaluations;
            ++stats_.partial_bounds.residual_degree_evaluations;
            if ((state_->maximum_residual_degree() + 1) / 2 > threshold_) {
                ++stats_.partial_bounds.residual_degree_prunes;
                record(prefix); return Outcome::failed;
            }
        } else if (options_.use_partial_bounds &&
            bounds_.completion_exceeds(std::span<const Graph::Mask>(&prefix, 1),
                static_cast<std::uint32_t>(path_.size()), threshold_, stats_.partial_bounds,
                std::nullopt, current_cut)) {
            record(prefix);
            return Outcome::failed;
        }

        struct Candidate { Graph::Vertex vertex; std::uint32_t cut; std::uint32_t memo_bound; };
        std::array<Candidate, 63> candidates{};
        std::size_t candidate_count = 0;
        const auto unplaced = all_ & ~prefix;
        auto consider = [&](Graph::Vertex v) {
            if ((twins_[v] & unplaced) != 0) { ++stats_.twin_symmetric_children_skipped; return; }
            const auto before = state_ ? state_->before(v) : static_cast<std::uint32_t>(std::popcount(graph_.adjacency(v) & prefix));
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(current_cut) + graph_.degree(v) - 2 * before);
            if (next <= threshold_) {
                std::uint32_t memo_bound = 0;
                if (options_.node_order == NodeOrder::memo && memo_gate) {
                    const auto child = oracle_->evaluate(prefix | (Graph::Mask{1} << v),
                        options_.node_memo_depth == 0 ? 0 : options_.node_memo_depth - 1,
                        deadline_, external_stop_);
                    memo_bound = child.bound;
                    if (child.complete && child.bound > threshold_) {
                        ++stats_.node_memo_child_rejections;
                        record_through(prefix | (Graph::Mask{1} << v), child.bound - 1);
                        return;
                    }
                }
                candidates[candidate_count++] = {v, next, memo_bound};
            }
            else ++stats_.children_rejected_by_cut;
        };
        const bool bucket_ordered = state_ && options_.node_order == NodeOrder::cut;
        if (bucket_ordered) {
            auto visit_bucket = [&](std::size_t bucket) {
                auto scan = state_->gain_bucket(bucket) & unplaced;
                while (scan) {
                    const auto v = static_cast<Graph::Vertex>(std::countr_zero(scan));
                    scan &= scan - 1; consider(v);
                }
            };
            if (options_.use_fail_first_candidate_order) {
                for (std::size_t bucket = state_->gain_bucket_count(); bucket-- > 0;)
                    visit_bucket(bucket);
            } else {
                for (std::size_t bucket = 0; bucket < state_->gain_bucket_count(); ++bucket)
                    visit_bucket(bucket);
            }
            ++stats_.node_sorts_avoided;
        } else {
            auto scan = unplaced;
            while (scan != 0) {
                const auto v = static_cast<Graph::Vertex>(std::countr_zero(scan));
                scan &= scan - 1; consider(v);
            }
        }
        if (options_.node_order == NodeOrder::memo) {
            std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(candidate_count),
                [](const Candidate& a, const Candidate& b) {
                    if (a.memo_bound != b.memo_bound) return a.memo_bound > b.memo_bound;
                    if (a.cut != b.cut) return a.cut > b.cut;
                    return a.vertex < b.vertex;
                });
        } else if (!bucket_ordered) {
            ++stats_.node_sorts_avoided;
            // Selection below preserves the old cut order without allocating or sorting.
            for (std::size_t i = 0; i < candidate_count; ++i) {
                std::size_t best = i;
                for (std::size_t j = i + 1; j < candidate_count; ++j) {
                    const bool better = options_.use_fail_first_candidate_order
                        ? (candidates[j].cut > candidates[best].cut)
                        : (candidates[j].cut < candidates[best].cut);
                    if (better || (candidates[j].cut == candidates[best].cut &&
                                   candidates[j].vertex < candidates[best].vertex)) best = j;
                }
                if (best != i) std::swap(candidates[i], candidates[best]);
            }
        }
        for (std::size_t candidate_index = 0; candidate_index < candidate_count; ++candidate_index) {
            const auto candidate = candidates[candidate_index];
            if (expired()) return Outcome::timed_out;
            const auto child_prefix = prefix | (Graph::Mask{1} << candidate.vertex);
            const auto remaining_after_child = graph_.size() - path_.size() - 1;
            const bool lookahead_gate = options_.depth_two_lookahead_max_remaining == 0 ||
                remaining_after_child <= options_.depth_two_lookahead_max_remaining;
            if (options_.use_depth_two_lookahead && lookahead_gate && child_prefix != all_) {
                const auto lookahead = has_legal_second(child_prefix, candidate.cut);
                if (lookahead == LookaheadOutcome::timed_out) return Outcome::timed_out;
                if (lookahead == LookaheadOutcome::no_legal_second) {
                    ++stats_.children_rejected_by_depth_two_lookahead;
                    continue;
                }
            }
            path_.push_back(candidate.vertex);
            if (state_) state_->push(candidate.vertex, candidate.cut);
            Outcome outcome;
            {
                NodeStateRollback rollback{state_.get(), candidate.vertex, current_cut};
                outcome = dfs(child_prefix, candidate.cut);
            }
            path_.pop_back();
            if (outcome != Outcome::failed) return outcome;
        }
        record(prefix);
        return Outcome::failed;
    }

    const Graph& graph_;
    std::uint32_t threshold_;
    DecisionOptions options_;
    Cache& cache_;
    Graph::Mask all_;
    Graph::Mask initial_prefix_;
    std::uint32_t initial_cut_;
    PartialBoundEvaluator bounds_;
    DecisionCacheStats cache_before_;
    Clock::time_point deadline_ = Clock::time_point::max();
    const std::atomic<bool>* external_stop_;
    bool collect_cache_delta_;
    std::vector<Graph::Mask> twins_;
    std::vector<Graph::Vertex> path_, solution_;
    DecisionStats stats_;
    std::uint64_t next_sdp_node_ = 0;
    std::unique_ptr<FiniteHorizonOracle> oracle_;
    std::unique_ptr<ReversibleNodeState64> state_;
    NodeMemoStats memo_before_;
};

bool equal_open(const Graph& graph, Graph::Vertex a, Graph::Vertex b) {
    return !graph.adjacent(a, b) && std::equal(
        graph.adjacency_words(a).begin(), graph.adjacency_words(a).end(),
        graph.adjacency_words(b).begin());
}

bool equal_closed(const Graph& graph, Graph::Vertex a, Graph::Vertex b) {
    if (!graph.adjacent(a, b)) return false;
    const auto aw = graph.adjacency_words(a), bw = graph.adjacency_words(b);
    for (std::size_t w = 0; w < graph.word_count(); ++w) {
        auto x = aw[w], y = bw[w];
        if (w == static_cast<std::size_t>(a) / 64U) x |= Graph::Mask{1} << (a % 64U);
        if (w == static_cast<std::size_t>(b) / 64U) y |= Graph::Mask{1} << (b % 64U);
        if (x != y) return false;
    }
    return true;
}

using DynamicTwinTable = std::vector<std::vector<Graph::Vertex>>;

std::shared_ptr<const DynamicTwinTable> build_dynamic_twins(
    const Graph& graph, bool enabled) {
    auto twins = std::make_shared<DynamicTwinTable>(graph.size());
    if (enabled) {
        for (Graph::Vertex later = 0; later < graph.size(); ++later)
            for (Graph::Vertex earlier = 0; earlier < later; ++earlier)
                if (equal_open(graph, earlier, later) ||
                    equal_closed(graph, earlier, later))
                    (*twins)[later].push_back(earlier);
    }
    return twins;
}

struct DynamicCandidate {
    Graph::Vertex vertex;
    std::uint32_t cut;
};

// Candidate vectors are recursive-frame state.  Keeping one vector per depth
// lets a worker reuse their allocations across all frontier tasks.
struct DynamicSearchScratch {
    explicit DynamicSearchScratch(std::size_t vertex_count)
        : candidates(vertex_count + 1) {}
    std::vector<std::vector<DynamicCandidate>> candidates;
};

// Scheduler/deadline observation stays off the recursive hot path except for
// one local decrement and branch. The block size moves geometrically until
// measured polling overhead meets the caller's dimensionless target. If the
// work between polls becomes slower, the block shrinks again, keeping response
// latency tied to measured overhead rather than a tuned time or node cutoff.
class GeometricControlPoller {
public:
    void reset(double overhead_fraction, Clock::time_point deadline,
               const std::atomic<bool>* external_stop) {
        if (!(overhead_fraction > 0.0 && overhead_fraction < 1.0))
            throw std::invalid_argument("control polling overhead fraction must be in (0,1)");
        fraction_ = overhead_fraction;
        deadline_ = deadline;
        external_stop_ = external_stop;
        active_ = external_stop_ != nullptr || deadline_ != Clock::time_point::max();
        stride_ = 1;
        remaining_ = 1;
        initialized_ = false;
    }

    bool stop_requested() {
        if (!active_) return false;
        if (--remaining_ != 0) return false;
        const auto before = Clock::now();
        const bool stop = (external_stop_ != nullptr &&
                external_stop_->load(std::memory_order_relaxed)) ||
            (deadline_ != Clock::time_point::max() && before >= deadline_);
        const auto after = Clock::now();
        if (initialized_) {
            const auto useful = before - previous_poll_completed_;
            const auto overhead = after - before;
            const auto useful_ticks = static_cast<long double>(useful.count());
            const auto overhead_ticks = static_cast<long double>(
                std::max<Clock::duration::rep>(1, overhead.count()));
            const auto target_ticks = overhead_ticks *
                static_cast<long double>(1.0 - fraction_) /
                static_cast<long double>(fraction_);
            if (useful_ticks < target_ticks &&
                stride_ <= std::numeric_limits<std::uint64_t>::max() / 2U) {
                stride_ *= 2U;
            } else if (useful_ticks > target_ticks * 4.0L && stride_ > 1U) {
                stride_ /= 2U;
            }
        }
        initialized_ = true;
        previous_poll_completed_ = after;
        remaining_ = stride_;
        return stop;
    }

private:
    double fraction_ = 0.01;
    Clock::time_point deadline_ = Clock::time_point::max();
    Clock::time_point previous_poll_completed_{};
    const std::atomic<bool>* external_stop_ = nullptr;
    std::uint64_t stride_ = 1;
    std::uint64_t remaining_ = 1;
    bool initialized_ = false;
    bool active_ = false;
};

template <typename Cache>
class SearchDynamic {
public:
    SearchDynamic(const Graph& graph, std::uint32_t threshold, DecisionOptions options,
                  Cache& cache,
                  std::vector<Graph::Vertex> initial_path = {},
                  std::uint32_t initial_cut = 0,
                  const std::atomic<bool>* external_stop = nullptr,
                  bool collect_cache_delta = true,
                  Clock::time_point absolute_deadline = Clock::time_point::max(),
                  std::shared_ptr<const DynamicTwinTable> twins = {},
                  DynamicSearchScratch* scratch = nullptr,
                  std::uint64_t maximum_nodes = std::numeric_limits<std::uint64_t>::max())
        : graph_(graph), threshold_(threshold), options_(options), cache_(cache),
          prefix_(graph.size()), before_(graph.size(), 0), bounds_(graph, options.partial_bounds, threshold),
          cache_before_(collect_cache_delta ? cache.stats() : DecisionCacheStats{}),
          path_(std::move(initial_path)), external_stop_(external_stop),
          collect_cache_delta_(collect_cache_delta), initial_cut_(initial_cut),
          twins_(twins ? std::move(twins) :
              build_dynamic_twins(graph, options.use_twin_symmetry)),
          owned_scratch_(scratch == nullptr
              ? std::make_unique<DynamicSearchScratch>(graph.size()) : nullptr),
          scratch_(scratch == nullptr ? owned_scratch_.get() : scratch),
          maximum_nodes_(maximum_nodes) {
        if (cache.word_count() != graph.word_count()) throw std::invalid_argument("dynamic cache belongs to a different graph size");
        for (const auto vertex : path_) {
            if (vertex >= graph_.size() || prefix_.contains(vertex))
                throw std::invalid_argument("invalid dynamic search prefix");
            prefix_.insert(vertex);
        }
        const auto prefix_words = prefix_.words();
        for (Graph::Vertex vertex = 0; vertex < graph_.size(); ++vertex) {
            if (prefix_.contains(vertex)) continue;
            const auto adjacency = graph_.adjacency_words(vertex);
            for (std::size_t word = 0; word < adjacency.size(); ++word)
                before_[vertex] += static_cast<std::uint32_t>(
                    std::popcount(adjacency[word] & prefix_words[word]));
        }
        if (absolute_deadline != Clock::time_point::max())
            deadline_ = absolute_deadline;
        else if (options_.time_limit.count() > 0)
            deadline_ = Clock::now() + options_.time_limit;
        control_poller_.reset(
            options_.controller_overhead_fraction, deadline_, external_stop_);
        if (options_.best_next_buckets) {
            std::uint32_t max_deg = 0;
            for (Graph::Vertex v = 0; v < graph_.size(); ++v) {
                max_deg = std::max(max_deg, graph_.degree(v));
            }
            histogram_.init(max_deg);
            for (Graph::Vertex v = 0; v < graph_.size(); ++v) {
                if (!prefix_.contains(v)) {
                    std::int32_t delta = static_cast<std::int32_t>(graph_.degree(v)) - 2 * static_cast<std::int32_t>(before_[v]);
                    histogram_.add(delta);
                }
            }
        }
        if (options_.candidate_enumerator != CandidateEnumerator::scan) {
            candidate_index_ = std::make_unique<DeltaBucketCandidateIndex>(
                graph_, prefix_, before_);
            if (options_.candidate_enumerator == CandidateEnumerator::cross_check)
                candidate_index_->validate(graph_, prefix_, before_);
        }
        if (options_.collect_dfs_diagnostics) {
            stats_.dfs_diagnostics.enabled = true;
            stats_.dfs_diagnostics.threshold = threshold_;
            stats_.dfs_diagnostics.by_depth.resize(graph_.size() + 1);
            DfsRootDiagnostics root;
            root.root_vertex = path_.empty() ? graph_.size() : path_.front();
            root.nodes_by_depth.resize(graph_.size() + 1);
            stats_.dfs_diagnostics.by_root.push_back(std::move(root));
        }
    }

    DecisionResult run() {
        path_.reserve(graph_.size());
        const auto outcome = dfs(initial_cut_);
        DecisionResult result;
        result.threshold = threshold_;
        result.status = outcome == Outcome::found ? DecisionStatus::feasible :
                        outcome == Outcome::failed ? DecisionStatus::infeasible : DecisionStatus::timed_out;
        if (outcome == Outcome::found) result.ordering = solution_;
        result.stats = stats_;
        if (collect_cache_delta_)
            copy_cache_delta(result.stats, cache_before_, cache_.stats());
        return result;
    }

private:
    DfsDepthDiagnostics* depth_diagnostics() noexcept {
        if (!stats_.dfs_diagnostics.enabled) return nullptr;
        return &stats_.dfs_diagnostics.by_depth[path_.size()];
    }
    DfsRootDiagnostics* root_diagnostics() noexcept {
        if (!stats_.dfs_diagnostics.enabled) return nullptr;
        return &stats_.dfs_diagnostics.by_root.front();
    }
    void record_node_diagnostics(std::uint32_t current_cut) noexcept {
        auto* depth = depth_diagnostics();
        if (!depth) return;
        ++stats_.dfs_diagnostics.nodes_entered;
        ++depth->nodes_entered;
        ++depth->slack_histogram[slack_bucket(threshold_ - current_cut)];
        auto* root = root_diagnostics();
        ++root->nodes_entered;
        root->maximum_depth = std::max(root->maximum_depth, path_.size());
        ++root->nodes_by_depth[path_.size()];
    }
    template <typename DepthMember, typename RootMember>
    void record_prune_diagnostics(DepthMember depth_member,
                                  RootMember root_member) noexcept {
        auto* depth = depth_diagnostics();
        if (!depth) return;
        ++(depth->*depth_member);
        auto* root = root_diagnostics();
        ++root->total_prunes;
        ++(root->*root_member);
    }
    void enumerate_scan_candidates(std::uint32_t current_cut,
                                   std::vector<DynamicCandidate>& candidates,
                                   bool record_stats) {
        if (record_stats)
            stats_.candidate_scan_checks += graph_.size() - path_.size();
        for (Graph::Vertex vertex = 0; vertex < graph_.size(); ++vertex) {
            if (prefix_.contains(vertex)) continue;
            if (twin_blocked(vertex)) {
                if (record_stats) {
                    ++stats_.twin_symmetric_children_skipped;
                    if (auto* depth = depth_diagnostics())
                        ++depth->children_rejected_by_symmetry;
                }
                continue;
            }
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(current_cut) + graph_.degree(vertex) -
                2 * before_[vertex]);
            if (next <= threshold_) candidates.push_back({vertex, next});
            else if (record_stats) {
                ++stats_.children_rejected_by_cut;
                if (auto* depth = depth_diagnostics())
                    ++depth->children_rejected_by_cut;
            }
        }
    }
    void enumerate_index_candidates(std::uint32_t current_cut,
                                    std::vector<DynamicCandidate>& candidates,
                                    bool record_stats) {
        if (!candidate_index_) throw std::logic_error("candidate index is unavailable");
        if (record_stats) ++stats_.candidate_index_gathers;
        const auto slots = candidate_index_->gather(
            static_cast<std::int32_t>(threshold_ - current_cut),
            [&](Graph::Vertex vertex, std::int32_t delta) {
                if (record_stats) ++stats_.candidate_index_vertices_emitted;
                if (twin_blocked(vertex)) {
                    if (record_stats) {
                        ++stats_.twin_symmetric_children_skipped;
                        if (auto* depth = depth_diagnostics())
                            ++depth->children_rejected_by_symmetry;
                    }
                    return;
                }
                const auto next = static_cast<std::int64_t>(current_cut) + delta;
                if (next < 0 || next > static_cast<std::int64_t>(threshold_))
                    throw std::logic_error("candidate index emitted an illegal vertex");
                candidates.push_back({vertex, static_cast<std::uint32_t>(next)});
            });
        if (record_stats) stats_.candidate_index_bucket_slots_visited += slots;
    }
    void enumerate_candidates(std::uint32_t current_cut,
                              std::vector<DynamicCandidate>& candidates) {
        if (options_.candidate_enumerator == CandidateEnumerator::scan) {
            enumerate_scan_candidates(current_cut, candidates, true);
            return;
        }
        enumerate_index_candidates(current_cut, candidates, true);
        if (options_.candidate_enumerator != CandidateEnumerator::cross_check)
            return;
        std::vector<DynamicCandidate> scanned;
        scanned.reserve(graph_.size());
        enumerate_scan_candidates(current_cut, scanned, false);
        auto indexed = candidates;
        const auto canonical_order = [](const DynamicCandidate& left,
                                        const DynamicCandidate& right) {
            if (left.vertex != right.vertex) return left.vertex < right.vertex;
            return left.cut < right.cut;
        };
        std::sort(scanned.begin(), scanned.end(), canonical_order);
        std::sort(indexed.begin(), indexed.end(), canonical_order);
        if (scanned.size() != indexed.size() ||
            !std::equal(scanned.begin(), scanned.end(), indexed.begin(),
                [](const DynamicCandidate& left, const DynamicCandidate& right) {
                    return left.vertex == right.vertex && left.cut == right.cut;
                }))
            throw std::logic_error("delta-bucket candidates differ from full scan");
        ++stats_.candidate_index_cross_checks;
    }
    bool expired() {
        const std::uint64_t total = stats_.nodes_expanded + stats_.residual_dp_states;
        const bool overflow = total < stats_.nodes_expanded;
        return overflow || total >= maximum_nodes_ ||
               control_poller_.stop_requested();
    }
    void record() {
        if (options_.use_failed_state_cache &&
            cache_.record_failed(prefix_.words(), threshold_))
            ++stats_.failed_states_recorded;
    }
    bool twin_blocked(Graph::Vertex v) const {
        for (const auto earlier : (*twins_)[v])
            if (!prefix_.contains(earlier)) return true;
        return false;
    }
    bool local_continuation_prunes(std::uint32_t current_cut,
                                   std::size_t viable_children) {
        if (options_.local_continuation_depth == 0) return false;
        if (threshold_ - current_cut > options_.local_continuation_max_slack) {
            ++stats_.local_continuation_slack_gate_skips;
            return false;
        }
        if (viable_children > options_.local_continuation_max_children) {
            ++stats_.local_continuation_branch_gate_skips;
            return false;
        }
        ++stats_.local_continuation_calls;
        if (!candidate_index_)
            throw std::logic_error("local continuation requires a candidate index");
        const auto started = Clock::now();
        LocalContinuationProbe probe(
            graph_, threshold_, prefix_, before_, *candidate_index_, *twins_,
            options_.candidate_enumerator == CandidateEnumerator::cross_check,
            options_.local_continuation_max_states);
        const auto result = probe.run(
            current_cut, options_.local_continuation_depth);
        stats_.local_continuation_nanoseconds +=
            static_cast<std::uint64_t>(std::chrono::duration_cast<
                std::chrono::nanoseconds>(Clock::now() - started).count());
        stats_.local_continuation_states += result.states;
        stats_.local_continuation_cross_checks += result.cross_checks;
        if (!result.conclusive) {
            ++stats_.local_continuation_inconclusive;
            return false;
        }
        if (result.continuation_exists) return false;
        ++stats_.local_continuation_parent_prunes;
        return true;
    }
    bool twin_blocked_after(Graph::Vertex v, Graph::Vertex newly_placed) const {
        for (const auto earlier : (*twins_)[v])
            if (earlier != newly_placed && !prefix_.contains(earlier)) return true;
        return false;
    }
    LookaheadOutcome has_legal_second(Graph::Vertex newly_placed,
                                      std::uint32_t current_cut) {
        ++stats_.depth_two_lookahead_checks;
        for (Graph::Vertex v = 0; v < graph_.size(); ++v) {
            if (expired()) return LookaheadOutcome::timed_out;
            if (v == newly_placed || prefix_.contains(v)) continue;
            if (twin_blocked_after(v, newly_placed)) continue;
            const auto before = before_[v] +
                static_cast<std::uint32_t>(graph_.adjacent(newly_placed, v));
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(current_cut) + graph_.degree(v) - 2 * before);
            if (next <= threshold_) return LookaheadOutcome::legal_second;
        }
        return LookaheadOutcome::no_legal_second;
    }
    Outcome dfs(std::uint32_t current_cut) {
        if (expired()) return Outcome::timed_out;
        ++stats_.nodes_expanded;
        record_node_diagnostics(current_cut);
        if (path_.size() == graph_.size()) { solution_ = path_; return Outcome::found; }
        if (options_.use_failed_state_cache) {
            ++stats_.failed_cache_queries;
            if (auto* depth = depth_diagnostics()) ++depth->cache_queries;
            const bool cache_hit = cache_.proves_failed(prefix_.words(), threshold_);
            if (cache_hit) {
                if (auto* depth = depth_diagnostics()) ++depth->cache_hits;
                record_prune_diagnostics(
                    &DfsDepthDiagnostics::cache_prunes,
                    &DfsRootDiagnostics::cache_prunes);
                return Outcome::failed;
            }
        }
        const auto residual = graph_.size() - path_.size();
        if (static_cast<std::uint32_t>(residual) < stats_.dfs_min_remaining_vertices) {
            stats_.dfs_min_remaining_vertices = static_cast<std::uint32_t>(residual);
        }
        if (options_.sdp_oracle && stats_.nodes_expanded >= next_sdp_node_ &&
            residual > 1 && options_.sdp_oracle->should_attempt(
                residual + 1, stats_.nodes_expanded)) {
                ++stats_.sdp_requests;
                const auto diagnostic_started = stats_.dfs_diagnostics.enabled
                    ? Clock::now() : Clock::time_point{};
                if (stats_.dfs_diagnostics.enabled)
                    ++stats_.dfs_diagnostics.sdp_bound.evaluations;
                next_sdp_node_ = stats_.nodes_expanded +
                    std::max<std::uint64_t>(1, options_.sdp_oracle->trigger_nodes());
                sdp::SdpBoundRequest request;
                request.prefix = prefix_.words();
                request.cardinality = residual / 2;
                request.accumulated_subtree_nodes = stats_.nodes_expanded;
                request.existing_certified_bound = threshold_;
                request.caller_deadline = deadline_;
                const auto bound = options_.sdp_oracle->bound(request);
                if (stats_.dfs_diagnostics.enabled)
                    stats_.dfs_diagnostics.sdp_bound.nanoseconds +=
                        static_cast<std::uint64_t>(std::chrono::duration_cast<
                            std::chrono::nanoseconds>(Clock::now() - diagnostic_started).count());
                if (bound.certified_lower_bound) {
                    ++stats_.sdp_certified;
                    if (*bound.certified_lower_bound > threshold_) {
                        ++stats_.sdp_prunes;
                        if (stats_.dfs_diagnostics.enabled)
                            ++stats_.dfs_diagnostics.sdp_bound.prunes;
                        record_prune_diagnostics(
                            &DfsDepthDiagnostics::sdp_prunes,
                            &DfsRootDiagnostics::bound_prunes);
                        record();
                        return Outcome::failed;
                    }
                }
        }
        if (options_.use_partial_bounds) {
            const auto diagnostic_started = stats_.dfs_diagnostics.enabled
                ? Clock::now() : Clock::time_point{};
            if (stats_.dfs_diagnostics.enabled)
                ++stats_.dfs_diagnostics.partial_bound.evaluations;
            const bool pruned = bounds_.completion_exceeds(
                prefix_.words(), static_cast<std::uint32_t>(path_.size()),
                threshold_, stats_.partial_bounds, std::nullopt, current_cut);
            if (stats_.dfs_diagnostics.enabled) {
                stats_.dfs_diagnostics.partial_bound.nanoseconds +=
                    static_cast<std::uint64_t>(std::chrono::duration_cast<
                        std::chrono::nanoseconds>(Clock::now() - diagnostic_started).count());
                if (pruned) ++stats_.dfs_diagnostics.partial_bound.prunes;
            }
            if (pruned) {
                record_prune_diagnostics(
                    &DfsDepthDiagnostics::partial_bound_prunes,
                    &DfsRootDiagnostics::bound_prunes);
                record();
                return Outcome::failed;
            }
        }
        if (residual >= 1 && residual <= options_.dfs_residual_dp_max_remaining) {
            ++stats_.residual_dp_attempts;
            if (stats_.dfs_diagnostics.enabled)
                ++stats_.dfs_diagnostics.residual_dp.evaluations;
            auto proj = project_residual_dp(residual, graph_.word_count());
            if (proj.has_value() && proj->peak_bytes <= options_.residual_dp_max_bytes && options_.memory_governor) {
                std::optional<ResidualDpSession> dp_session;
                dp_session.emplace(graph_, prefix_.words(), options_.memory_governor);
                if (dp_session->applicable()) {
                    ++stats_.residual_dp_admissions;
                    stats_.residual_dp_peak_bytes = std::max(stats_.residual_dp_peak_bytes, static_cast<std::uint64_t>(proj->peak_bytes));

                    bool completed = false;
                    while (!completed) {
                        if (expired()) {
                            return Outcome::timed_out;
                        }
                        std::uint64_t chunk = 1024;
                        if (maximum_nodes_ != std::numeric_limits<std::uint64_t>::max()) {
                            std::uint64_t current_total = stats_.nodes_expanded + stats_.residual_dp_states;
                            if (current_total >= maximum_nodes_) {
                                return Outcome::timed_out;
                            }
                            chunk = std::min<std::uint64_t>(chunk, maximum_nodes_ - current_total);
                        }
                        const auto dp_started = std::chrono::steady_clock::now();
                        auto event = dp_session->service(chunk, deadline_);
                        const auto dp_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - dp_started).count();
                        stats_.residual_dp_seconds += dp_elapsed;
                        if (stats_.dfs_diagnostics.enabled)
                            stats_.dfs_diagnostics.residual_dp.nanoseconds +=
                                static_cast<std::uint64_t>(std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(Clock::now() - dp_started).count());
                        stats_.residual_dp_states += event.states_completed;
                        if (event.complete) {
                            completed = true;
                            if (*event.exact_completion > threshold_) {
                                ++stats_.residual_dp_completed_tails;
                                ++stats_.residual_dp_infeasible_prunes;
                                if (stats_.dfs_diagnostics.enabled)
                                    ++stats_.dfs_diagnostics.residual_dp.prunes;
                                record_prune_diagnostics(
                                    &DfsDepthDiagnostics::residual_dp_prunes,
                                    &DfsRootDiagnostics::bound_prunes);
                                record();
                                return Outcome::failed;
                            } else {
                                ++stats_.residual_dp_completed_tails;
                                ++stats_.residual_dp_feasible_witnesses;
                                auto remaining_ordering = dp_session->reconstruct_witness();
                                std::vector<Graph::Vertex> full_ordering = path_;
                                full_ordering.insert(full_ordering.end(), remaining_ordering.begin(), remaining_ordering.end());
                                if (!graph_.validate_ordering(full_ordering) || graph_.ordering_cutwidth(full_ordering) > threshold_) {
                                    throw std::logic_error("residual DP reconstructed an invalid witness");
                                }
                                solution_ = full_ordering;
                                return Outcome::found;
                            }
                        }
                    }
                } else {
                    ++stats_.residual_dp_governor_or_cap_rejections;
                }
            } else {
                ++stats_.residual_dp_governor_or_cap_rejections;
            }
        }
        if (options_.best_next_buckets) {
            ++stats_.best_next_bucket_checks;
            const auto diagnostic_started = stats_.dfs_diagnostics.enabled
                ? Clock::now() : Clock::time_point{};
            if (stats_.dfs_diagnostics.enabled)
                ++stats_.dfs_diagnostics.best_next_bound.evaluations;
            std::int32_t min_delta = histogram_.get_min_delta();
            if (stats_.dfs_diagnostics.enabled)
                stats_.dfs_diagnostics.best_next_bound.nanoseconds +=
                    static_cast<std::uint64_t>(std::chrono::duration_cast<
                        std::chrono::nanoseconds>(Clock::now() - diagnostic_started).count());
            if (min_delta != std::numeric_limits<std::int32_t>::max()) {
                if (static_cast<std::int64_t>(current_cut) + min_delta > static_cast<std::int64_t>(threshold_)) {
                    ++stats_.best_next_bucket_parent_prunes;
                    if (stats_.dfs_diagnostics.enabled)
                        ++stats_.dfs_diagnostics.best_next_bound.prunes;
                    record_prune_diagnostics(
                        &DfsDepthDiagnostics::best_next_prunes,
                        &DfsRootDiagnostics::bound_prunes);
                    stats_.best_next_bucket_candidates_avoided += (graph_.size() - path_.size());
                    record();
                    return Outcome::failed;
                }
            }
        }
        auto& candidates = scratch_->candidates[path_.size()];
        candidates.clear();
        if (candidates.capacity() < graph_.size()) candidates.reserve(graph_.size());
        enumerate_candidates(current_cut, candidates);
        if (auto* depth = depth_diagnostics()) {
            ++depth->viable_child_observations;
            depth->viable_children_sum += candidates.size();
            ++depth->viable_child_histogram[viable_child_bucket(candidates.size())];
            if (candidates.empty()) {
                ++depth->dead_ends;
                auto* root = root_diagnostics();
                ++root->dead_ends;
                ++root->total_prunes;
            }
        }
        std::sort(candidates.begin(), candidates.end(), [this](
            const DynamicCandidate& a, const DynamicCandidate& b) {
            if (a.cut != b.cut)
                return options_.use_fail_first_candidate_order ? a.cut > b.cut : a.cut < b.cut;
            return a.vertex < b.vertex;
        });
        if (local_continuation_prunes(current_cut, candidates.size())) {
            record();
            return Outcome::failed;
        }
        for (const auto candidate : candidates) {
            if (expired()) return Outcome::timed_out;
            const auto remaining_after_child = graph_.size() - path_.size() - 1;
            const bool lookahead_gate = options_.depth_two_lookahead_max_remaining == 0 ||
                remaining_after_child <= options_.depth_two_lookahead_max_remaining;
            if (options_.use_depth_two_lookahead && lookahead_gate &&
                path_.size() + 1 < graph_.size()) {
                const auto lookahead = has_legal_second(candidate.vertex, candidate.cut);
                if (lookahead == LookaheadOutcome::timed_out) return Outcome::timed_out;
                if (lookahead == LookaheadOutcome::no_legal_second) {
                    ++stats_.children_rejected_by_depth_two_lookahead;
                    if (auto* depth = depth_diagnostics())
                        ++depth->children_rejected_by_lookahead;
                    continue;
                }
            }
            if (options_.best_next_buckets) {
                histogram_.remove(static_cast<std::int32_t>(graph_.degree(candidate.vertex)) - 2 * static_cast<std::int32_t>(before_[candidate.vertex]));
            }
            if (candidate_index_) candidate_index_->remove(candidate.vertex);
            prefix_.insert(candidate.vertex);
            path_.push_back(candidate.vertex);
            const auto adjacency = graph_.adjacency_words(candidate.vertex);
            for (std::size_t word_index = 0; word_index < adjacency.size(); ++word_index) {
                auto word = adjacency[word_index];
                while (word) {
                    const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
                    word &= word - 1;
                    const auto other = static_cast<Graph::Vertex>(word_index * 64U + bit);
                    if (other < graph_.size() && !prefix_.contains(other)) {
                        if (options_.best_next_buckets) {
                            histogram_.remove(static_cast<std::int32_t>(graph_.degree(other)) - 2 * static_cast<std::int32_t>(before_[other]));
                        }
                        if (candidate_index_) {
                            const auto delta = static_cast<std::int32_t>(graph_.degree(other)) -
                                2 * static_cast<std::int32_t>(before_[other]);
                            candidate_index_->move(other, delta - 2);
                            ++stats_.candidate_index_forward_updates;
                        }
                        ++before_[other]; ++stats_.node_state_updates;
                        if (options_.best_next_buckets) {
                            histogram_.add(static_cast<std::int32_t>(graph_.degree(other)) - 2 * static_cast<std::int32_t>(before_[other]));
                        }
                    }
                }
            }
            if (options_.candidate_enumerator == CandidateEnumerator::cross_check)
                candidate_index_->validate(graph_, prefix_, before_);
            const auto outcome = dfs(candidate.cut);
            for (std::size_t word_index = 0; word_index < adjacency.size(); ++word_index) {
                auto word = adjacency[word_index];
                while (word) {
                    const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
                    word &= word - 1;
                    const auto other = static_cast<Graph::Vertex>(word_index * 64U + bit);
                    if (other < graph_.size() && !prefix_.contains(other)) {
                        if (options_.best_next_buckets) {
                            histogram_.remove(static_cast<std::int32_t>(graph_.degree(other)) - 2 * static_cast<std::int32_t>(before_[other]));
                        }
                        --before_[other]; ++stats_.node_state_updates;
                        if (candidate_index_) {
                            const auto delta = static_cast<std::int32_t>(graph_.degree(other)) -
                                2 * static_cast<std::int32_t>(before_[other]);
                            candidate_index_->move(other, delta);
                            ++stats_.candidate_index_rollback_updates;
                        }
                        if (options_.best_next_buckets) {
                            histogram_.add(static_cast<std::int32_t>(graph_.degree(other)) - 2 * static_cast<std::int32_t>(before_[other]));
                        }
                    }
                }
            }
            path_.pop_back();
            prefix_.erase(candidate.vertex);
            if (options_.best_next_buckets) {
                histogram_.add(static_cast<std::int32_t>(graph_.degree(candidate.vertex)) - 2 * static_cast<std::int32_t>(before_[candidate.vertex]));
            }
            if (candidate_index_) {
                candidate_index_->insert(candidate.vertex,
                    static_cast<std::int32_t>(graph_.degree(candidate.vertex)) -
                    2 * static_cast<std::int32_t>(before_[candidate.vertex]));
            }
            if (options_.candidate_enumerator == CandidateEnumerator::cross_check)
                candidate_index_->validate(graph_, prefix_, before_);
            if (outcome != Outcome::failed) return outcome;
        }
        record();
        return Outcome::failed;
    }

    const Graph& graph_;
    std::uint32_t threshold_;
    DecisionOptions options_;
    Cache& cache_;
    VertexSet prefix_;
    std::vector<std::uint32_t> before_;
    PartialBoundEvaluator bounds_;
    DecisionCacheStats cache_before_;
    Clock::time_point deadline_ = Clock::time_point::max();
    std::vector<Graph::Vertex> path_, solution_;
    const std::atomic<bool>* external_stop_ = nullptr;
    bool collect_cache_delta_ = true;
    std::uint32_t initial_cut_ = 0;
    DecisionStats stats_;
    std::uint64_t next_sdp_node_ = 0;
    std::shared_ptr<const DynamicTwinTable> twins_;
    std::unique_ptr<DynamicSearchScratch> owned_scratch_;
    DynamicSearchScratch* scratch_ = nullptr;
    GeometricControlPoller control_poller_;
    std::uint64_t maximum_nodes_ = std::numeric_limits<std::uint64_t>::max();
    DeltaHistogram histogram_;
    std::unique_ptr<DeltaBucketCandidateIndex> candidate_index_;
};

} // namespace

class RecursiveDynamicSubtreeWorker::Impl {
public:
    class Runner {
    public:
        virtual ~Runner() = default;
        virtual DecisionResult run(
            const std::vector<Graph::Vertex>& path, std::uint32_t cut,
            const std::atomic<bool>* external_stop,
            Clock::time_point absolute_deadline,
            std::uint64_t maximum_nodes) = 0;
    };

    template <typename Cache>
    class TypedRunner final : public Runner {
    public:
        TypedRunner(const Graph& graph, std::uint32_t threshold,
                    DecisionOptions options, Cache& cache)
            : graph_(graph), threshold_(threshold), options_(std::move(options)),
              cache_(cache),
              twins_(build_dynamic_twins(graph, options_.use_twin_symmetry)),
              scratch_(graph.size()) {
            options_.threads = 1;
            options_.shared_twins.reset();
            if (cache_.word_count() != graph_.word_count())
                throw std::invalid_argument(
                    "recursive subtree cache has wrong graph size");
        }

        DecisionResult run(const std::vector<Graph::Vertex>& path,
                           std::uint32_t cut,
                           const std::atomic<bool>* external_stop,
                           Clock::time_point absolute_deadline,
                           std::uint64_t maximum_nodes) override {
            VertexSet prefix(graph_.size());
            for (const auto vertex : path) {
                if (vertex >= graph_.size() || prefix.contains(vertex))
                    throw std::invalid_argument(
                        "recursive subtree has invalid prefix");
                prefix.insert(vertex);
            }
            std::uint32_t recomputed_cut = 0;
            for (Graph::Vertex vertex = 0; vertex < graph_.size(); ++vertex) {
                if (!prefix.contains(vertex)) continue;
                const auto adjacency = graph_.adjacency_words(vertex);
                for (std::size_t word = 0; word < adjacency.size(); ++word)
                    recomputed_cut += static_cast<std::uint32_t>(
                        std::popcount(adjacency[word] & ~prefix.words()[word]));
            }
            if (recomputed_cut != cut)
                throw std::invalid_argument(
                    "recursive subtree prefix/cut mismatch");

            auto local_options = options_;
            if (absolute_deadline != Clock::time_point::max() &&
                local_options.time_limit.count() == 0)
                local_options.time_limit = std::chrono::milliseconds{1};
            SearchDynamic search(graph_, threshold_, std::move(local_options), cache_,
                path, cut, external_stop, false, absolute_deadline, twins_, &scratch_,
                maximum_nodes);
            return search.run();
        }

    private:
        const Graph& graph_;
        std::uint32_t threshold_;
        DecisionOptions options_;
        Cache& cache_;
        std::shared_ptr<const DynamicTwinTable> twins_;
        DynamicSearchScratch scratch_;
    };

    Impl(const Graph& graph, std::uint32_t threshold, DecisionOptions options,
         ShardedDynamicDecisionCache& cache)
        : runner_(std::make_unique<TypedRunner<ShardedDynamicDecisionCache>>(
              graph, threshold, std::move(options), cache)) {}

    Impl(const Graph& graph, std::uint32_t threshold, DecisionOptions options,
         ShardedFixedThresholdDynamicCache& cache)
        : runner_(std::make_unique<TypedRunner<ShardedFixedThresholdDynamicCache>>(
              graph, threshold, std::move(options), cache)) {}

    DecisionResult run(const std::vector<Graph::Vertex>& path, std::uint32_t cut,
                       const std::atomic<bool>* external_stop,
                       Clock::time_point absolute_deadline,
                       std::uint64_t maximum_nodes) {
        return runner_->run(path, cut, external_stop, absolute_deadline, maximum_nodes);
    }

    std::unique_ptr<Runner> runner_;
};

RecursiveDynamicSubtreeWorker::RecursiveDynamicSubtreeWorker(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    ShardedDynamicDecisionCache& cache)
    : impl_(std::make_unique<Impl>(
          graph, threshold, std::move(options), cache)) {}
RecursiveDynamicSubtreeWorker::RecursiveDynamicSubtreeWorker(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    ShardedFixedThresholdDynamicCache& cache)
    : impl_(std::make_unique<Impl>(
          graph, threshold, std::move(options), cache)) {}
RecursiveDynamicSubtreeWorker::~RecursiveDynamicSubtreeWorker() = default;
RecursiveDynamicSubtreeWorker::RecursiveDynamicSubtreeWorker(
    RecursiveDynamicSubtreeWorker&&) noexcept = default;
RecursiveDynamicSubtreeWorker& RecursiveDynamicSubtreeWorker::operator=(
    RecursiveDynamicSubtreeWorker&&) noexcept = default;
DecisionResult RecursiveDynamicSubtreeWorker::run(
    const std::vector<Graph::Vertex>& prefix, std::uint32_t cut,
    const std::atomic<bool>* external_stop,
    std::chrono::steady_clock::time_point absolute_deadline,
    std::uint64_t maximum_nodes) {
    return impl_->run(prefix, cut, external_stop, absolute_deadline, maximum_nodes);
}

namespace {

void add_stats(DecisionStats& total, const DecisionStats& part,
               bool sum_cache_memory = false) {
    total.parallel_workers_used = std::max(
        total.parallel_workers_used, part.parallel_workers_used);
    total.parallel_root_tasks_started += part.parallel_root_tasks_started;
    total.parallel_root_tasks_completed += part.parallel_root_tasks_completed;
    total.nodes_expanded += part.nodes_expanded;
    total.children_rejected_by_cut += part.children_rejected_by_cut;
    total.failed_cache_hits += part.failed_cache_hits;
    total.failed_cache_queries += part.failed_cache_queries;
    total.failed_states_recorded += part.failed_states_recorded;
    total.twin_symmetric_children_skipped += part.twin_symmetric_children_skipped;
    total.depth_two_lookahead_checks += part.depth_two_lookahead_checks;
    total.children_rejected_by_depth_two_lookahead += part.children_rejected_by_depth_two_lookahead;
    for (std::size_t d = 0; d < total.node_memo_hits_by_depth.size(); ++d)
        total.node_memo_hits_by_depth[d] += part.node_memo_hits_by_depth[d];
    total.node_memo_computations += part.node_memo_computations;
    total.node_memo_prunes += part.node_memo_prunes;
    total.node_memo_child_rejections += part.node_memo_child_rejections;
    total.node_memo_collisions += part.node_memo_collisions;
    total.node_memo_saturation += part.node_memo_saturation;
    total.node_memo_memory_bytes = std::max(total.node_memo_memory_bytes,
                                            part.node_memo_memory_bytes);
    total.node_memo_available = total.node_memo_available || part.node_memo_available;
    total.node_state_updates += part.node_state_updates;
    total.node_sorts_avoided += part.node_sorts_avoided;
    if (sum_cache_memory) {
        total.failed_state_cache_size += part.failed_state_cache_size;
        total.failed_state_cache_capacity += part.failed_state_cache_capacity;
        total.failed_state_cache_memory_bytes += part.failed_state_cache_memory_bytes;
    } else {
        total.failed_state_cache_size = std::max(
            total.failed_state_cache_size, part.failed_state_cache_size);
        total.failed_state_cache_capacity = std::max(
            total.failed_state_cache_capacity, part.failed_state_cache_capacity);
        total.failed_state_cache_memory_bytes = std::max(
            total.failed_state_cache_memory_bytes, part.failed_state_cache_memory_bytes);
    }
    total.failed_state_bounds_strengthened += part.failed_state_bounds_strengthened;
    total.failed_state_insertions_skipped += part.failed_state_insertions_skipped;
    total.cache_collisions += part.cache_collisions;
    total.cache_page_promotions += part.cache_page_promotions;
    total.cache_page_second_chances += part.cache_page_second_chances;
    total.cache_pages_recycled += part.cache_pages_recycled;
    total.cache_replacement_admissions += part.cache_replacement_admissions;
    total.cache_entries_evicted += part.cache_entries_evicted;
    total.cache_evicted_depth_sum += part.cache_evicted_depth_sum;
    total.cache_maximum_evicted_depth = std::max(
        total.cache_maximum_evicted_depth, part.cache_maximum_evicted_depth);
    total.partial_bounds.evaluations += part.partial_bounds.evaluations;
    total.partial_bounds.residual_degree_evaluations += part.partial_bounds.residual_degree_evaluations;
    total.partial_bounds.edge_distance_area_evaluations += part.partial_bounds.edge_distance_area_evaluations;
    total.partial_bounds.degree_distance_area_evaluations += part.partial_bounds.degree_distance_area_evaluations;
    total.partial_bounds.degeneracy_evaluations += part.partial_bounds.degeneracy_evaluations;
    total.partial_bounds.residual_degree_prunes += part.partial_bounds.residual_degree_prunes;
    total.partial_bounds.edge_distance_area_prunes += part.partial_bounds.edge_distance_area_prunes;
    total.partial_bounds.degree_distance_area_prunes += part.partial_bounds.degree_distance_area_prunes;
    total.partial_bounds.degeneracy_prunes += part.partial_bounds.degeneracy_prunes;
    total.partial_bounds.expensive_slack_gate_skips +=
        part.partial_bounds.expensive_slack_gate_skips;
    total.sdp_requests += part.sdp_requests;
    total.sdp_certified += part.sdp_certified;
    total.sdp_prunes += part.sdp_prunes;
    total.configured_proof_regions_bound = std::max(total.configured_proof_regions_bound, part.configured_proof_regions_bound);
    total.resolved_proof_regions_bound = std::max(total.resolved_proof_regions_bound, part.resolved_proof_regions_bound);
    total.peak_proof_regions = std::max(total.peak_proof_regions, part.peak_proof_regions);
    total.suppressed_donations += part.suppressed_donations;
    total.residual_dp_attempts += part.residual_dp_attempts;
    total.residual_dp_admissions += part.residual_dp_admissions;
    total.residual_dp_governor_or_cap_rejections += part.residual_dp_governor_or_cap_rejections;
    total.residual_dp_completed_tails += part.residual_dp_completed_tails;
    total.residual_dp_infeasible_prunes += part.residual_dp_infeasible_prunes;
    total.residual_dp_feasible_witnesses += part.residual_dp_feasible_witnesses;
    total.residual_dp_states += part.residual_dp_states;
    total.residual_dp_peak_bytes = std::max(total.residual_dp_peak_bytes, part.residual_dp_peak_bytes);
    total.residual_dp_seconds += part.residual_dp_seconds;
    total.residual_dp_cold_restarts += part.residual_dp_cold_restarts;
    total.dfs_min_remaining_vertices = std::min(total.dfs_min_remaining_vertices, part.dfs_min_remaining_vertices);
    total.best_next_bucket_checks += part.best_next_bucket_checks;
    total.best_next_bucket_parent_prunes += part.best_next_bucket_parent_prunes;
    total.best_next_bucket_candidates_avoided += part.best_next_bucket_candidates_avoided;
    total.candidate_scan_checks += part.candidate_scan_checks;
    total.candidate_index_gathers += part.candidate_index_gathers;
    total.candidate_index_bucket_slots_visited +=
        part.candidate_index_bucket_slots_visited;
    total.candidate_index_vertices_emitted += part.candidate_index_vertices_emitted;
    total.candidate_index_forward_updates += part.candidate_index_forward_updates;
    total.candidate_index_rollback_updates += part.candidate_index_rollback_updates;
    total.candidate_index_cross_checks += part.candidate_index_cross_checks;
    total.local_continuation_calls += part.local_continuation_calls;
    total.local_continuation_slack_gate_skips +=
        part.local_continuation_slack_gate_skips;
    total.local_continuation_branch_gate_skips +=
        part.local_continuation_branch_gate_skips;
    total.local_continuation_inconclusive += part.local_continuation_inconclusive;
    total.local_continuation_states += part.local_continuation_states;
    total.local_continuation_parent_prunes += part.local_continuation_parent_prunes;
    total.local_continuation_nanoseconds += part.local_continuation_nanoseconds;
    total.local_continuation_cross_checks += part.local_continuation_cross_checks;
    merge_dfs_diagnostics(total.dfs_diagnostics, part.dfs_diagnostics);
}

DecisionResult decide_word64_parallel(const Graph& graph, std::uint32_t threshold,
                                      DecisionOptions options) {
    if (graph.size() == 0) {
        DecisionResult result;
        result.status = DecisionStatus::feasible;
        result.threshold = threshold;
        return result;
    }
    std::vector<Graph::Vertex> roots;
    for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex) {
        if (graph.degree(vertex) > threshold) continue;
        bool twin_blocked = false;
        if (options.use_twin_symmetry) {
            for (Graph::Vertex earlier = 0; earlier < vertex; ++earlier) {
                const auto earlier_bit = Graph::Mask{1} << earlier;
                const auto vertex_bit = Graph::Mask{1} << vertex;
                if (graph.adjacency(earlier) == graph.adjacency(vertex) ||
                    (graph.adjacency(earlier) | earlier_bit) ==
                        (graph.adjacency(vertex) | vertex_bit)) {
                    twin_blocked = true;
                    break;
                }
            }
        }
        if (!twin_blocked) roots.push_back(vertex);
    }
    DecisionResult combined;
    combined.threshold = threshold;
    if (roots.empty()) {
        combined.status = DecisionStatus::infeasible;
        return combined;
    }

    const std::size_t worker_count = std::min(options.threads, roots.size());
    if (options.parallel_min_cache_shards == 0 ||
        options.parallel_cache_shards_per_thread == 0)
        throw std::invalid_argument("parallel cache shard policy must be positive");
    const std::size_t shard_count = std::max(
        options.parallel_min_cache_shards,
        worker_count * options.parallel_cache_shards_per_thread);
    std::unique_ptr<ShardedWord64DecisionCache> cross_cache;
    std::unique_ptr<ShardedFixedThresholdWord64Cache> fixed_cache;
    if (options.cache_mode == CacheMode::fixed_threshold)
        fixed_cache = std::make_unique<ShardedFixedThresholdWord64Cache>(
            shard_count, threshold, cache_options(options));
    else
        cross_cache = std::make_unique<ShardedWord64DecisionCache>(
            shard_count, cache_options(options));
    const auto started = Clock::now();
    const auto deadline = options.time_limit.count() == 0
        ? Clock::time_point::max() : started + options.time_limit;
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> tasks_started{0};
    std::atomic<std::uint64_t> tasks_completed{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> timed_out{false};
    std::mutex result_mutex;
    std::vector<DecisionStats> worker_stats(worker_count);
    std::vector<Graph::Vertex> witness;
    std::exception_ptr failure;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                auto worker_options = options;
                worker_options.threads = 1;
                while (!stop.load(std::memory_order_relaxed)) {
                    const auto task = next.fetch_add(1, std::memory_order_relaxed);
                    if (task >= roots.size()) break;
                    if (deadline != Clock::time_point::max()) {
                        const auto now = Clock::now();
                        if (now >= deadline) {
                            timed_out.store(true, std::memory_order_relaxed);
                            stop.store(true, std::memory_order_relaxed);
                            break;
                        }
                        worker_options.time_limit = std::max(
                            std::chrono::milliseconds{1},
                            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
                    }
                    const auto vertex = roots[task];
                    tasks_started.fetch_add(1, std::memory_order_relaxed);
                    DecisionResult result;
                    if (fixed_cache) {
                        Search64 search(graph, threshold, worker_options, *fixed_cache,
                            Graph::Mask{1} << vertex, graph.degree(vertex),
                            {vertex}, &stop, false);
                        result = search.run();
                    } else {
                        Search64 search(graph, threshold, worker_options, *cross_cache,
                            Graph::Mask{1} << vertex, graph.degree(vertex),
                            {vertex}, &stop, false);
                        result = search.run();
                    }
                    add_stats(worker_stats[worker], result.stats);
                    if (result.status != DecisionStatus::timed_out)
                        tasks_completed.fetch_add(1, std::memory_order_relaxed);
                    if (result.status == DecisionStatus::feasible) {
                        {
                            std::lock_guard lock(result_mutex);
                            if (witness.empty() || result.ordering < witness)
                                witness = std::move(result.ordering);
                        }
                        stop.store(true, std::memory_order_relaxed);
                    } else if (result.status == DecisionStatus::timed_out &&
                               !stop.load(std::memory_order_relaxed)) {
                        timed_out.store(true, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                std::lock_guard lock(result_mutex);
                if (!failure) failure = std::current_exception();
                stop.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    if (failure) std::rethrow_exception(failure);
    for (const auto& stats : worker_stats) add_stats(combined.stats, stats, true);
    combined.stats.parallel_workers_used = worker_count;
    combined.stats.parallel_root_tasks_started =
        tasks_started.load(std::memory_order_relaxed);
    combined.stats.parallel_root_tasks_completed =
        tasks_completed.load(std::memory_order_relaxed);
    const auto cache_stats = fixed_cache ? fixed_cache->stats() : cross_cache->stats();
    combined.stats.failed_cache_queries = cache_stats.queries;
    combined.stats.failed_cache_hits = cache_stats.hits;
    combined.stats.failed_states_recorded = cache_stats.inserts;
    combined.stats.failed_state_bounds_strengthened = cache_stats.strengthenings;
    combined.stats.failed_state_insertions_skipped = cache_stats.rejected_capacity;
    combined.stats.cache_collisions = cache_stats.collisions;
    combined.stats.cache_page_promotions = cache_stats.page_promotions;
    combined.stats.cache_page_second_chances = cache_stats.page_second_chances;
    combined.stats.cache_pages_recycled = cache_stats.pages_recycled;
    combined.stats.cache_replacement_admissions = cache_stats.replacement_admissions;
    combined.stats.cache_entries_evicted = cache_stats.entries_evicted;
    combined.stats.cache_evicted_depth_sum = cache_stats.evicted_depth_sum;
    combined.stats.cache_maximum_evicted_depth = cache_stats.maximum_evicted_depth;
    combined.stats.failed_state_cache_size = cache_stats.entries;
    combined.stats.failed_state_cache_capacity = cache_stats.capacity;
    combined.stats.failed_state_cache_memory_bytes = cache_stats.memory_bytes;
    if (!witness.empty()) {
        combined.status = DecisionStatus::feasible;
        combined.ordering = std::move(witness);
    } else if (timed_out.load(std::memory_order_relaxed)) {
        combined.status = DecisionStatus::timed_out;
    } else {
        combined.status = DecisionStatus::infeasible;
    }
    return combined;
}

DecisionResult decide_dynamic_parallel(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    ShardedDynamicDecisionCache* shared_cache = nullptr) {
    if (graph.size() == 0) {
        DecisionResult result;
        result.status = DecisionStatus::feasible;
        result.threshold = threshold;
        return result;
    }
    const auto twins = build_dynamic_twins(graph, options.use_twin_symmetry);
    std::vector<Graph::Vertex> roots;
    for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex) {
        if (graph.degree(vertex) > threshold) continue;
        if ((*twins)[vertex].empty()) roots.push_back(vertex);
    }
    DecisionResult combined;
    combined.threshold = threshold;
    if (roots.empty()) {
        combined.status = DecisionStatus::infeasible;
        return combined;
    }
    if (graph.size() == 1) {
        combined.status = DecisionStatus::feasible;
        combined.ordering = {roots.front()};
        return combined;
    }

    struct FrontierTask {
        Graph::Vertex first;
        Graph::Vertex second;
        std::uint32_t cut;
    };
    std::vector<FrontierTask> tasks;
    tasks.reserve(roots.size() * std::min<std::size_t>(graph.size(), 64));
    std::unordered_set<std::uint64_t> frontier_prefixes;
    frontier_prefixes.reserve(roots.size() * std::min<std::size_t>(graph.size(), 64));
    for (const auto first : roots) {
        const auto first_cut = graph.degree(first);
        for (Graph::Vertex second = 0; second < graph.size(); ++second) {
            if (second == first) continue;
            bool twin_blocked = false;
            for (const auto earlier : (*twins)[second]) {
                if (earlier != first) {
                    twin_blocked = true;
                    break;
                }
            }
            if (twin_blocked) continue;
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(first_cut) + graph.degree(second) -
                2 * static_cast<std::uint32_t>(graph.adjacent(first, second)));
            if (next <= threshold) {
                const auto low = std::min(first, second);
                const auto high = std::max(first, second);
                const auto key = (static_cast<std::uint64_t>(low) << 32U) | high;
                if (frontier_prefixes.insert(key).second)
                    tasks.push_back({first, second, next});
            }
        }
    }
    if (tasks.empty()) {
        combined.status = DecisionStatus::infeasible;
        return combined;
    }
    std::sort(tasks.begin(), tasks.end(), [&](const FrontierTask& a,
                                               const FrontierTask& b) {
        if (a.cut != b.cut)
            return options.use_fail_first_candidate_order ? a.cut > b.cut : a.cut < b.cut;
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });

    const std::size_t worker_count = std::min(options.threads, tasks.size());
    if (options.parallel_min_cache_shards == 0 ||
        options.parallel_cache_shards_per_thread == 0)
        throw std::invalid_argument("parallel cache shard policy must be positive");
    const std::size_t requested_shards = std::max(
        options.parallel_min_cache_shards,
        worker_count * options.parallel_cache_shards_per_thread);
    std::unique_ptr<ShardedDynamicDecisionCache> owned_cache;
    if (shared_cache == nullptr) {
        owned_cache = std::make_unique<ShardedDynamicDecisionCache>(
            graph.word_count(), requested_shards, cache_options(options));
        shared_cache = owned_cache.get();
    } else if (shared_cache->word_count() != graph.word_count()) {
        throw std::invalid_argument("dynamic cache belongs to a different graph size");
    }
    auto& cache = *shared_cache;
    const auto cache_before = cache.stats();
    const auto started = Clock::now();
    const auto deadline = options.time_limit.count() == 0
        ? Clock::time_point::max() : started + options.time_limit;
    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> tasks_started{0};
    std::atomic<std::uint64_t> tasks_completed{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> timed_out{false};
    std::mutex result_mutex;
    std::vector<DecisionStats> worker_stats(worker_count);
    std::vector<Graph::Vertex> witness;
    std::exception_ptr failure;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                auto worker_options = options;
                worker_options.threads = 1;
                DynamicSearchScratch scratch(graph.size());
                while (!stop.load(std::memory_order_relaxed)) {
                    const auto task = next.fetch_add(1, std::memory_order_relaxed);
                    if (task >= tasks.size()) break;
                    if (deadline != Clock::time_point::max()) {
                        const auto now = Clock::now();
                        if (now >= deadline) {
                            timed_out.store(true, std::memory_order_relaxed);
                            stop.store(true, std::memory_order_relaxed);
                            break;
                        }
                        worker_options.time_limit = std::max(
                            std::chrono::milliseconds{1},
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now));
                    }
                    const auto frontier = tasks[task];
                    tasks_started.fetch_add(1, std::memory_order_relaxed);
                    SearchDynamic search(
                        graph, threshold, worker_options, cache,
                        std::vector<Graph::Vertex>{frontier.first, frontier.second},
                        frontier.cut, &stop, false, deadline, twins, &scratch);
                    auto result = search.run();
                    add_stats(worker_stats[worker], result.stats);
                    if (result.status != DecisionStatus::timed_out)
                        tasks_completed.fetch_add(1, std::memory_order_relaxed);
                    if (result.status == DecisionStatus::feasible) {
                        {
                            std::lock_guard lock(result_mutex);
                            if (witness.empty()) witness = std::move(result.ordering);
                        }
                        stop.store(true, std::memory_order_relaxed);
                    } else if (result.status == DecisionStatus::timed_out &&
                               !stop.load(std::memory_order_relaxed)) {
                        timed_out.store(true, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                std::lock_guard lock(result_mutex);
                if (!failure) failure = std::current_exception();
                stop.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    if (failure) std::rethrow_exception(failure);
    for (const auto& stats : worker_stats) add_stats(combined.stats, stats, true);
    combined.stats.parallel_workers_used = worker_count;
    combined.stats.parallel_root_tasks_started =
        tasks_started.load(std::memory_order_relaxed);
    combined.stats.parallel_root_tasks_completed =
        tasks_completed.load(std::memory_order_relaxed);
    const auto cache_stats = cache.stats();
    combined.stats.failed_cache_queries = cache_stats.queries - cache_before.queries;
    combined.stats.failed_cache_hits = cache_stats.hits - cache_before.hits;
    combined.stats.failed_states_recorded = cache_stats.inserts - cache_before.inserts;
    combined.stats.failed_state_bounds_strengthened =
        cache_stats.strengthenings - cache_before.strengthenings;
    combined.stats.failed_state_insertions_skipped =
        cache_stats.rejected_capacity - cache_before.rejected_capacity;
    combined.stats.cache_collisions = cache_stats.collisions - cache_before.collisions;
    combined.stats.cache_page_promotions = cache_stats.page_promotions - cache_before.page_promotions;
    combined.stats.cache_page_second_chances = cache_stats.page_second_chances - cache_before.page_second_chances;
    combined.stats.cache_pages_recycled = cache_stats.pages_recycled - cache_before.pages_recycled;
    combined.stats.cache_replacement_admissions = cache_stats.replacement_admissions - cache_before.replacement_admissions;
    combined.stats.cache_entries_evicted = cache_stats.entries_evicted - cache_before.entries_evicted;
    combined.stats.cache_evicted_depth_sum = cache_stats.evicted_depth_sum - cache_before.evicted_depth_sum;
    combined.stats.cache_maximum_evicted_depth = cache_stats.maximum_evicted_depth;
    combined.stats.failed_state_cache_size = cache_stats.entries;
    combined.stats.failed_state_cache_capacity = cache_stats.capacity;
    combined.stats.failed_state_cache_memory_bytes = cache_stats.memory_bytes;
    if (!witness.empty()) {
        combined.status = DecisionStatus::feasible;
        combined.ordering = std::move(witness);
    } else if (timed_out.load(std::memory_order_relaxed)) {
        combined.status = DecisionStatus::timed_out;
    } else {
        combined.status = DecisionStatus::infeasible;
    }
    return combined;
}

} // namespace

DecisionResult decide_cutwidth_cached(const Graph& graph, std::uint32_t threshold,
                                      DecisionOptions options, Word64DecisionCache& cache) {
    return Search64(graph, threshold, options, cache).run();
}

DecisionResult decide_cutwidth_cached(const Graph& graph, std::uint32_t threshold,
                                      DecisionOptions options, DynamicDecisionCache& cache) {
    return SearchDynamic(graph, threshold, options, cache).run();
}

DecisionResult decide_cutwidth_cached(
    const Graph& graph, std::uint32_t threshold, DecisionOptions options,
    ShardedDynamicDecisionCache& cache) {
    if (options.threads > 1)
        return decide_dynamic_parallel(graph, threshold, options, &cache);
    return SearchDynamic(graph, threshold, options, cache).run();
}

DecisionResult decide_cutwidth_cached(const Graph& graph, std::uint32_t threshold,
                                      DecisionOptions options, FixedThresholdWord64Cache& cache) {
    return Search64(graph, threshold, options, cache).run();
}

DecisionResult decide_cutwidth(const Graph& graph, std::uint32_t threshold,
                               DecisionOptions options) {
    if (options.threads == 0) throw std::invalid_argument("decision thread count must be positive");
    if (graph.size() == 0) {
        DecisionResult result;
        result.status = DecisionStatus::feasible;
        result.threshold = threshold;
        return result;
    }
    if (options.node_memo_depth > 4)
        throw std::invalid_argument("node memo depth must be between 0 and 4");
    if (options.node_memo_depth != 0 && !options.node_memo &&
        options.failed_state_cache_memory_bytes != 0 &&
        options.node_memo_memory_bytes > options.failed_state_cache_memory_bytes)
        throw std::invalid_argument("node memo memory exceeds cache memory");
    if (options.node_memo_depth != 0 && graph.supports_mask() && !options.node_memo &&
        options.node_memo_memory_bytes != 0) {
        try { options.node_memo = std::make_shared<NodeMemoTable>(options.node_memo_memory_bytes); }
        catch (const std::bad_alloc&) { options.node_memo.reset(); }
        if (options.node_memo && options.failed_state_cache_memory_bytes != 0)
            options.failed_state_cache_memory_bytes -= options.node_memo_memory_bytes;
    }
    const bool use_word64 = options.backend == DecisionBackend::word64 ||
        (options.backend == DecisionBackend::automatic && graph.supports_mask());
    if (use_word64) {
        if (!graph.supports_mask())
            throw std::invalid_argument("word64 backend does not support more than 63 vertices");
        if (options.threads > 1)
            return decide_word64_parallel(graph, threshold, options);
        Word64DecisionCache cache(cache_options(options));
        return decide_cutwidth_cached(graph, threshold, options, cache);
    }
    if (options.threads > 1)
        return decide_dynamic_parallel(graph, threshold, options);
    DynamicDecisionCache cache(graph.word_count(), cache_options(options));
    return decide_cutwidth_cached(graph, threshold, options, cache);
}

} // namespace cutwidth
