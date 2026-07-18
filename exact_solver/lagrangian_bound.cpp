#include "lagrangian_bound.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <vector>

namespace cutwidth {
namespace {

struct FlowEdge {
    int to;
    int64_t cap;
    int64_t flow;
    size_t rev;
};

class Dinic {
public:
    explicit Dinic(int num_vertices)
        : num_vertices_(num_vertices), adj_(num_vertices), level_(num_vertices), ptr_(num_vertices) {}

    void add_edge(int from, int to, int64_t cap) {
        if (cap <= 0) return;
        adj_[from].push_back(FlowEdge{to, cap, 0, adj_[to].size()});
        adj_[to].push_back(FlowEdge{from, 0, 0, adj_[from].size() - 1});
    }

    int64_t max_flow(int s, int t) {
        int64_t flow = 0;
        while (bfs(s, t)) {
            std::fill(ptr_.begin(), ptr_.end(), 0);
            while (int64_t pushed = dfs(s, t, INT64_MAX)) {
                flow += pushed;
            }
        }
        return flow;
    }

private:
    bool bfs(int s, int t) {
        std::fill(level_.begin(), level_.end(), -1);
        level_[s] = 0;
        std::vector<int> q;
        q.reserve(num_vertices_);
        q.push_back(s);
        size_t head = 0;
        while (head < q.size()) {
            int v = q[head++];
            for (const auto& edge : adj_[v]) {
                if (edge.cap - edge.flow > 0 && level_[edge.to] == -1) {
                    level_[edge.to] = level_[v] + 1;
                    q.push_back(edge.to);
                }
            }
        }
        return level_[t] != -1;
    }

    int64_t dfs(int v, int t, int64_t pushed) {
        if (pushed == 0) return 0;
        if (v == t) return pushed;
        for (size_t& cid = ptr_[v]; cid < adj_[v].size(); ++cid) {
            auto& edge = adj_[v][cid];
            int trg = edge.to;
            if (level_[v] + 1 != level_[trg] || edge.cap - edge.flow == 0) continue;
            int64_t trg_pushed = dfs(trg, t, std::min(pushed, edge.cap - edge.flow));
            if (trg_pushed == 0) continue;
            edge.flow += trg_pushed;
            adj_[trg][edge.rev].flow -= trg_pushed;
            return trg_pushed;
        }
        return 0;
    }

    int num_vertices_;
    std::vector<std::vector<FlowEdge>> adj_;
    std::vector<int> level_;
    std::vector<size_t> ptr_;
};

inline bool checked_add(int64_t a, int64_t b, int64_t& result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &result);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return true;
    }
    result = a + b;
    return false;
#endif
}

inline bool checked_sub(int64_t a, int64_t b, int64_t& result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, &result);
#else
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) {
        return true;
    }
    result = a - b;
    return false;
#endif
}

inline bool checked_mul(int64_t a, int64_t b, int64_t& result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, &result);
#else
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) return true;
        } else {
            if (b < INT64_MIN / a) return true;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) return true;
        } else {
            if (a != 0 && b < INT64_MAX / a) return true;
        }
    }
    result = a * b;
    return false;
#endif
}

inline int64_t ceil_div(int64_t x, int64_t y) {
    // Assumes y > 0
    int64_t q = x / y;
    int64_t r = x % y;
    if (r > 0) {
        return q + 1;
    }
    return q;
}

} // namespace

LagrangianPrefixBoundEvaluator::LagrangianPrefixBoundEvaluator(const Graph& graph)
    : graph_(graph), max_degree_(0) {
    for (Graph::Vertex v = 0; v < graph.size(); ++v) {
        max_degree_ = std::max(max_degree_, graph.degree(v));
    }
}

LagrangianTelemetry LagrangianPrefixBoundEvaluator::evaluate(
    Graph::Mask prefix,
    std::optional<std::uint32_t> threshold,
    const std::vector<std::uint32_t>& selected_cardinalities,
    std::optional<std::uint32_t> lambda_denominator,
    std::optional<std::vector<int64_t>> lambda_numerators
) const {
    std::vector<Graph::Mask> prefix_words(graph_.word_count(), 0);
    if (!prefix_words.empty()) {
        prefix_words[0] = prefix;
    }
    return evaluate(prefix_words, threshold, selected_cardinalities, lambda_denominator, lambda_numerators);
}

LagrangianTelemetry LagrangianPrefixBoundEvaluator::evaluate(
    std::span<const Graph::Mask> prefix,
    std::optional<std::uint32_t> threshold,
    const std::vector<std::uint32_t>& selected_cardinalities,
    std::optional<std::uint32_t> lambda_denominator,
    std::optional<std::vector<int64_t>> lambda_numerators
) const {
    LagrangianTelemetry tel;
    tel.calls = 1;

    const std::size_t N = graph_.size();
    const std::size_t word_count = graph_.word_count();

    // 1. Identify the residual set U = V \ S.  This evaluator returns a
    // lower bound on q_t itself, not on the maximum cut already seen along
    // the prefix; the controller may combine those two distinct facts.
    std::vector<Graph::Vertex> U;
    U.reserve(N);
    for (Graph::Vertex v = 0; v < N; ++v) {
        std::size_t word_idx = v / 64U;
        std::size_t bit_idx = v % 64U;
        bool in_S = false;
        if (word_idx < prefix.size()) {
            in_S = (prefix[word_idx] & (Graph::Mask{1} << bit_idx)) != 0;
        }
        if (!in_S) {
            U.push_back(v);
        }
    }

    const std::size_t U_size = U.size();
    if (U_size <= 1) {
        tel.ineligible = true;
        return tel;
    }

    // 2. Set up the denominator q
    std::uint64_t q_val = lambda_denominator.value_or(2U);
    if (q_val == 0) {
        tel.ineligible = true;
        return tel;
    }
    int64_t q = static_cast<int64_t>(q_val);
    if (q < 0) {
        tel.ineligible = true;
        return tel;
    }

    // 3. Set up the cardinalities t to scan
    std::vector<std::uint32_t> t_list;
    if (!selected_cardinalities.empty()) {
        for (std::uint32_t t : selected_cardinalities) {
            if (t >= 1 && t < U_size) {
                t_list.push_back(t);
            }
        }
    } else {
        t_list.reserve(U_size - 1);
        for (std::uint32_t t = 1; t < U_size; ++t) {
            t_list.push_back(t);
        }
    }

    if (t_list.empty()) {
        tel.ineligible = true;
        return tel;
    }

    // 4. Set up the numerators p to scan
    std::vector<int64_t> p_list;
    if (lambda_numerators.has_value()) {
        p_list = lambda_numerators.value();
    } else {
        // Default range: [-q * max_degree_, q * max_degree_]
        int64_t limit;
        if (checked_mul(q, static_cast<int64_t>(max_degree_), limit)) {
            tel.overflow = true;
            return tel;
        }
        // Safely bounds-check limit allocation sizing
        if (limit > 100000) {
            tel.overflow = true;
            return tel;
        }
        p_list.reserve(static_cast<size_t>(2 * limit + 1));
        for (int64_t p = -limit; p <= limit; ++p) {
            p_list.push_back(p);
        }
    }

    // Precompute a_v = |E(v, S)| for each v in U
    std::vector<std::uint32_t> a(U_size, 0);
    for (std::size_t i = 0; i < U_size; ++i) {
        Graph::Vertex v = U[i];
        const auto neighbors = graph_.adjacency_words(v);
        std::uint32_t val = 0;
        for (std::size_t w = 0; w < word_count; ++w) {
            if (w < prefix.size()) {
                val += static_cast<std::uint32_t>(std::popcount(neighbors[w] & prefix[w]));
            }
        }
        a[i] = val;
    }

    // Precompute the residual graph edges in G[U]
    std::vector<std::vector<std::pair<int, int64_t>>> U_adj(U_size);
    for (std::size_t i = 0; i < U_size; ++i) {
        Graph::Vertex u = U[i];
        const auto neighbors = graph_.adjacency_words(u);
        for (std::size_t j = i + 1; j < U_size; ++j) {
            Graph::Vertex v = U[j];
            std::size_t v_word = v / 64U;
            std::size_t v_bit = v % 64U;
            bool adjacent = false;
            if (v_word < word_count) {
                adjacent = (neighbors[v_word] & (Graph::Mask{1} << v_bit)) != 0;
            }
            if (adjacent) {
                U_adj[i].emplace_back(static_cast<int>(j), q);
                U_adj[j].emplace_back(static_cast<int>(i), q);
            }
        }
    }

    // 5. Scan grid points.
    std::uint32_t best_bound = 0;

    for (std::uint32_t t : t_list) {
        for (int64_t p : p_list) {
            tel.mincuts++;

            if (q >= INT64_MAX / 4) {
                tel.overflow = true;
                continue;
            }

            // Calculate scaled constant: q * sum(a_v) - p * t
            int64_t sum_a = 0;
            for (std::uint32_t val : a) {
                sum_a += val;
            }

            int64_t q_sum_a;
            if (checked_mul(q, sum_a, q_sum_a)) {
                tel.overflow = true;
                continue;
            }

            int64_t p_t;
            if (checked_mul(p, static_cast<int64_t>(t), p_t)) {
                tel.overflow = true;
                continue;
            }

            int64_t scaled_constant;
            if (checked_sub(q_sum_a, p_t, scaled_constant)) {
                tel.overflow = true;
                continue;
            }

            // Construct the flow network
            Dinic dinic(static_cast<int>(U_size + 2));

            // Add G[U] edges
            for (std::size_t i = 0; i < U_size; ++i) {
                for (const auto& [j, cap] : U_adj[i]) {
                    if (i < static_cast<std::size_t>(j)) {
                        dinic.add_edge(static_cast<int>(i + 2), static_cast<int>(j + 2), cap);
                        dinic.add_edge(static_cast<int>(j + 2), static_cast<int>(i + 2), cap);
                    }
                }
            }

            // Add source/sink edges and update scaled constant
            int64_t sum_cap_s = 0;
            int64_t sum_cap_t = 0;
            bool point_overflow = false;

            for (std::size_t i = 0; i < U_size; ++i) {
                int64_t q_a;
                if (checked_mul(q, static_cast<int64_t>(a[i]), q_a)) {
                    point_overflow = true;
                    break;
                }

                int64_t w_v;
                if (checked_sub(p, q_a, w_v)) {
                    point_overflow = true;
                    break;
                }

                if (w_v >= 0) {
                    if (checked_add(sum_cap_t, w_v, sum_cap_t)) {
                        point_overflow = true;
                        break;
                    }
                    dinic.add_edge(static_cast<int>(i + 2), 1, w_v);
                } else {
                    if (checked_add(scaled_constant, w_v, scaled_constant)) {
                        point_overflow = true;
                        break;
                    }
                    int64_t neg_w_v;
                    if (checked_sub(0, w_v, neg_w_v)) {
                        point_overflow = true;
                        break;
                    }
                    if (checked_add(sum_cap_s, neg_w_v, sum_cap_s)) {
                        point_overflow = true;
                        break;
                    }
                    dinic.add_edge(0, static_cast<int>(i + 2), neg_w_v);
                }
            }

            if (point_overflow || sum_cap_s >= INT64_MAX / 4 || sum_cap_t >= INT64_MAX / 4) {
                tel.overflow = true;
                continue;
            }

            // Compute min-cut
            int64_t min_cut = dinic.max_flow(0, 1);

            int64_t total_scaled;
            if (checked_add(scaled_constant, min_cut, total_scaled)) {
                tel.overflow = true;
                continue;
            }

            int64_t bound_val = ceil_div(total_scaled, q);
            std::uint32_t final_bound = 0;
            if (bound_val > 0) {
                if (bound_val > static_cast<int64_t>(UINT32_MAX)) {
                    tel.overflow = true;
                    continue;
                }
                final_bound = static_cast<std::uint32_t>(bound_val);
            }
            if (final_bound > best_bound) {
                best_bound = final_bound;
                tel.best_cardinality = t;
                tel.best_numerator = p;
                tel.best_denominator = q;
                tel.certified_bound = best_bound;

                if (threshold.has_value() && best_bound > threshold.value()) {
                    return tel;
                }
            }
        }
    }

    return tel;
}

} // namespace cutwidth
