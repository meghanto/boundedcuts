#include "sdp_bound_oracle.hpp"

#include "clarabel_sdp_adapter.hpp"

#include <algorithm>
#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cutwidth::sdp {
namespace {

using Clock = std::chrono::steady_clock;

void hash_word(std::uint64_t& hash, std::uint64_t value) {
    for (unsigned byte = 0; byte != 8; ++byte) {
        hash ^= (value >> (8 * byte)) & 0xffU;
        hash *= 1099511628211ULL;
    }
}

std::string hex_hash(std::uint64_t hash) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::string compute_graph_hash(const Graph& graph) {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_word(hash, graph.size());
    for (Graph::Vertex v = 0; v < graph.size(); ++v)
        for (const auto word : graph.adjacency_words(v)) hash_word(hash, word);
    return hex_hash(hash);
}

bool valid_prefix(const Graph& graph, std::span<const Graph::Mask> prefix) {
    if (prefix.size() != graph.word_count()) return false;
    if (prefix.empty() || graph.size() % 64U == 0) return true;
    return (prefix.back() >> (graph.size() % 64U)) == 0;
}

std::size_t prefix_size(std::span<const Graph::Mask> prefix) {
    std::size_t result = 0;
    for (const auto word : prefix) result += std::popcount(word);
    return result;
}

std::string model_key(const std::string& graph_hash,
                      std::span<const Graph::Mask> prefix,
                      std::size_t cardinality) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : graph_hash) hash_word(hash, static_cast<unsigned char>(c));
    hash_word(hash, 1); // weighted-model schema version
    for (const auto word : prefix) hash_word(hash, word);
    hash_word(hash, cardinality);
    return hex_hash(hash);
}

#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
bool contains(std::span<const Graph::Mask> prefix, Graph::Vertex vertex) {
    return (prefix[vertex / 64U] & (Graph::Mask{1} << (vertex % 64U))) != 0;
}

struct ResidualModel {
    Graph graph;
    std::vector<std::uint32_t> boundary_weights;
};

ResidualModel make_residual(const Graph& parent,
                            std::span<const Graph::Mask> prefix) {
    std::vector<Graph::Vertex> residual;
    std::vector<Graph::Vertex> local(parent.size(), std::numeric_limits<Graph::Vertex>::max());
    for (Graph::Vertex v = 0; v < parent.size(); ++v) {
        if (!contains(prefix, v)) {
            local[v] = static_cast<Graph::Vertex>(residual.size());
            residual.push_back(v);
        }
    }
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    std::vector<std::uint32_t> boundary(residual.size(), 0);
    for (std::size_t i = 0; i < residual.size(); ++i) {
        const auto v = residual[i];
        for (Graph::Vertex u = 0; u < parent.size(); ++u) {
            if (contains(prefix, u) && parent.adjacent(v, u)) ++boundary[i];
            if (!contains(prefix, u) && v < u && parent.adjacent(v, u))
                edges.emplace_back(static_cast<Graph::Vertex>(i), local[u]);
        }
    }
    return {Graph(residual.size(), edges), std::move(boundary)};
}

const char* proof_name(BisectionProofKind kind) {
    return kind == BisectionProofKind::exact_shifted_bareiss
        ? "exact_shifted_bareiss" : "rowwise_diagonal_dominance";
}
#endif

} // namespace

SdpBoundOracle::SdpBoundOracle(const Graph& graph, SdpBoundOracleOptions options)
    : graph_(graph), options_(options), graph_hash_(compute_graph_hash(graph)) {
    if (options_.quantization_bits < 2 || options_.quantization_bits > 50)
        throw std::invalid_argument("SDP oracle quantization bits must be between 2 and 50");
    preferred_max_dimension_.store(
        options_.max_state_dimension == 0 ? graph.size() + 1 : options_.max_state_dimension,
        std::memory_order_relaxed);
}

SdpBoundResult SdpBoundOracle::bound(const SdpBoundRequest& request) {
    ++requests_;
    SdpBoundResult out;
    out.graph_hash = graph_hash_;
    out.backend_hash = "clarabel.cpp-v0.11.1-weighted-v1";
    if (options_.schedule == SdpSchedule::off) {
        out.status = SdpBoundStatus::disabled;
        return out;
    }
    if (!valid_prefix(graph_, request.prefix)) {
        out.status = SdpBoundStatus::invalid_request;
        out.diagnostic = "prefix word count or high bits do not match graph";
        return out;
    }
    const auto residual_size = graph_.size() - prefix_size(request.prefix);
    if (request.cardinality == 0 || request.cardinality >= residual_size) {
        out.status = SdpBoundStatus::invalid_request;
        out.diagnostic = "cardinality must lie strictly inside the residual state";
        return out;
    }
    out.model_hash = model_key(graph_hash_, request.prefix, request.cardinality);
    {
        std::lock_guard lock(cache_mutex_);
        const auto found = cache_.find(out.model_hash);
        if (found != cache_.end()) {
            ++cache_hits_;
            out.status = SdpBoundStatus::cache_hit;
            out.cache_hit = true;
            out.certified_lower_bound = found->second.lower_bound;
            out.proof_kind = found->second.proof_kind;
            return out;
        }
    }
    if (!request.root && options_.schedule != SdpSchedule::adaptive) {
        out.status = SdpBoundStatus::ineligible;
        return out;
    }
    const std::uint64_t trigger = options_.trigger_nodes;
    if (!request.root && request.accumulated_subtree_nodes < trigger) {
        out.status = SdpBoundStatus::ineligible;
        return out;
    }
    ++eligible_;
    const auto preferred_dimension = preferred_max_dimension_.load(std::memory_order_relaxed);
    if ((options_.max_state_dimension != 0 &&
         residual_size + 1 > options_.max_state_dimension) ||
        (!request.root && residual_size + 1 > preferred_dimension)) {
        ++dimension_rejections_;
        out.status = SdpBoundStatus::dimension_exceeded;
        return out;
    }
    auto deadline = request.caller_deadline;
    if (options_.total_time.count() != 0) {
        const auto budget_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            options_.total_time).count();
        const auto used_ns = backend_nanoseconds_.load(std::memory_order_relaxed);
        if (used_ns >= static_cast<std::uint64_t>(budget_ns)) {
            ++budget_rejections_;
            out.status = SdpBoundStatus::time_budget_exhausted;
            return out;
        }
        const auto remaining_ns = static_cast<std::uint64_t>(budget_ns) - used_ns;
        const auto completed_calls = calls_.load(std::memory_order_relaxed);
        const auto remaining_calls = options_.max_calls == 0
            ? std::size_t{4}
            : std::max<std::size_t>(1, options_.max_calls -
                std::min<std::size_t>(options_.max_calls, completed_calls));
        // A root or early state must not monopolize the global SDP budget.
        // Reserve an equal share for the remaining configured calls. With an
        // unlimited call count, use a rolling four-call horizon.
        const auto slice_ns = std::max<std::uint64_t>(1000000,
            remaining_ns / remaining_calls);
        deadline = std::min(deadline, Clock::now() +
            std::chrono::nanoseconds(std::min(remaining_ns, slice_ns)));
    }
    if (Clock::now() >= deadline) {
        ++budget_rejections_;
        out.status = SdpBoundStatus::time_budget_exhausted;
        return out;
    }
    auto observed = calls_.load(std::memory_order_relaxed);
    do {
        if (options_.max_calls != 0 && observed >= options_.max_calls) {
            ++budget_rejections_;
            out.status = SdpBoundStatus::call_budget_exhausted;
            return out;
        }
    } while (!calls_.compare_exchange_weak(observed, observed + 1,
                                            std::memory_order_relaxed));
    std::unique_lock backend_lock(backend_mutex_, std::try_to_lock);
    if (!backend_lock.owns_lock()) {
        // A busy rejection is not a backend call and must not consume quota.
        calls_.fetch_sub(1, std::memory_order_relaxed);
        ++busy_;
        out.status = SdpBoundStatus::busy;
        return out;
    }
#ifndef CUTWIDTH_HAVE_CLARABEL_SDP
    calls_.fetch_sub(1, std::memory_order_relaxed);
    out.status = SdpBoundStatus::unavailable;
    out.diagnostic = "Clarabel.cpp SDP backend was not configured";
    return out;
#else
    const auto call_started = Clock::now();
    const auto model = make_residual(graph_, request.prefix);
    const auto before_solve = Clock::now();
    if (before_solve >= deadline) {
        out.status = SdpBoundStatus::time_budget_exhausted;
        return out;
    }
    ClarabelWeightedBisectionOptions options;
    options.cardinality = request.cardinality;
    options.max_iterations = options_.max_iterations;
    options.quantization_bits = options_.quantization_bits;
    options.time_limit_seconds = std::max(0.001,
        std::chrono::duration<double>(deadline - before_solve).count());
    const auto solved = solve_weighted_bisection_sdp_clarabel(
        model.graph, model.boundary_weights, options);
    const auto finished = Clock::now();
    backend_nanoseconds_.fetch_add(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - call_started).count()),
        std::memory_order_relaxed);
    out.raw_objective = solved.raw_dual_bound;
    out.primal_residual = solved.primal_residual;
    out.dual_residual = solved.dual_residual;
    out.iterations = solved.iterations;
    out.solve_seconds = solved.solve_seconds;
    out.diagnostic = solved.diagnostic;
    if (!solved.certificate.valid || !solved.certificate.integer_lower_bound ||
        !verify_weighted_bisection_certificate(
            model.graph, model.boundary_weights, solved.certificate)) {
        ++uncertified_;
        if (!request.root) {
            const auto dimension = residual_size + 1;
            const auto smaller = std::max<std::size_t>(8, dimension - std::max<std::size_t>(1, dimension / 4));
            auto current = preferred_max_dimension_.load(std::memory_order_relaxed);
            while (smaller < current && !preferred_max_dimension_.compare_exchange_weak(
                current, smaller, std::memory_order_relaxed)) {}
        }
        out.status = SdpBoundStatus::uncertified;
        return out;
    }
    out.status = SdpBoundStatus::certified;
    out.certified_lower_bound = *solved.certificate.integer_lower_bound;
    out.proof_kind = proof_name(solved.certificate.proof_kind);
    ++certified_;
    const bool improvement = *out.certified_lower_bound > request.existing_certified_bound;
    if (improvement) {
        ++improvements_;
        preferred_max_dimension_.store(
            options_.max_state_dimension == 0 ? graph_.size() + 1 : options_.max_state_dimension,
            std::memory_order_relaxed);
    } else if (!request.root) {
        const auto dimension = residual_size + 1;
        const auto smaller = std::max<std::size_t>(8, dimension - std::max<std::size_t>(1, dimension / 4));
        auto current = preferred_max_dimension_.load(std::memory_order_relaxed);
        while (smaller < current && !preferred_max_dimension_.compare_exchange_weak(
            current, smaller, std::memory_order_relaxed)) {}
    }
    {
        std::lock_guard lock(cache_mutex_);
        cache_.emplace(out.model_hash, CachedBound{
            *out.certified_lower_bound, out.proof_kind, out.model_hash});
    }
    return out;
#endif
}

bool SdpBoundOracle::should_attempt(std::size_t state_dimension,
                                    std::uint64_t accumulated_nodes) const noexcept {
    if (options_.schedule != SdpSchedule::adaptive ||
        accumulated_nodes < options_.trigger_nodes) return false;
    if (options_.max_state_dimension != 0 &&
        state_dimension > options_.max_state_dimension) return false;
    return state_dimension <= preferred_max_dimension_.load(std::memory_order_relaxed);
}

SdpBoundOracleStats SdpBoundOracle::stats() const {
    SdpBoundOracleStats out;
    out.requests = requests_.load(std::memory_order_relaxed);
    out.eligible = eligible_.load(std::memory_order_relaxed);
    out.calls = calls_.load(std::memory_order_relaxed);
    out.cache_hits = cache_hits_.load(std::memory_order_relaxed);
    out.certified = certified_.load(std::memory_order_relaxed);
    out.improvements = improvements_.load(std::memory_order_relaxed);
    out.busy = busy_.load(std::memory_order_relaxed);
    out.budget_rejections = budget_rejections_.load(std::memory_order_relaxed);
    out.uncertified = uncertified_.load(std::memory_order_relaxed);
    out.dimension_rejections = dimension_rejections_.load(std::memory_order_relaxed);
    out.preferred_max_dimension = preferred_max_dimension_.load(std::memory_order_relaxed);
    out.backend_seconds = static_cast<double>(
        backend_nanoseconds_.load(std::memory_order_relaxed)) / 1.0e9;
    return out;
}

} // namespace cutwidth::sdp
