#include "pb_encoding.hpp"

#include <algorithm>
#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace cutwidth::pb {
namespace {

using Literal = std::int32_t;

class Builder {
public:
    explicit Builder(CardinalityEncoding encoding) : encoding_(encoding) {}

    std::uint32_t variable() {
        if (formula.variable_count == static_cast<std::uint32_t>(
                std::numeric_limits<Literal>::max()))
            throw std::overflow_error("CNF variable limit exceeded");
        return ++formula.variable_count;
    }

    void clause(std::initializer_list<Literal> literals) {
        formula.clauses.emplace_back(literals);
    }

    void clause(std::vector<Literal> literals) {
        formula.clauses.push_back(std::move(literals));
    }

    void at_most(const std::vector<Literal>& literals, std::size_t bound) {
        if (bound >= literals.size()) return;
        if (bound == 0) {
            for (const auto literal : literals) clause({-literal});
            return;
        }
        const auto output = unary_count(literals, bound + 1);
        clause({-output[bound]});
    }

    void exactly_one(const std::vector<Literal>& literals) {
        if (literals.empty()) {
            clause(std::vector<Literal>{});
            return;
        }
        clause(literals);
        at_most(literals, 1);
    }

    std::vector<Literal> unary_count(
        const std::vector<Literal>& literals, std::size_t limit) {
        if (literals.empty() || limit == 0) return {};
        limit = std::min(limit, literals.size());
        return encoding_ == CardinalityEncoding::sequential_counter
            ? sequential_count(literals, limit)
            : totalizer(literals, 0, literals.size(), limit);
    }

    CnfFormula formula;

private:
    std::vector<Literal> sequential_count(
        const std::vector<Literal>& x, std::size_t limit) {
        // s[i][j] means at least j+1 of x[0..i] are true. Only forward
        // implications are needed when -s[n-1][bound] is asserted.
        std::vector<std::vector<Literal>> s(x.size(),
            std::vector<Literal>(limit, 0));
        for (std::size_t i = 0; i < x.size(); ++i)
            for (std::size_t j = 0; j < std::min(i + 1, limit); ++j)
                s[i][j] = static_cast<Literal>(variable());
        for (std::size_t i = 0; i < x.size(); ++i) {
            clause({-x[i], s[i][0]});
            if (i == 0) continue;
            for (std::size_t j = 0; j < std::min(i, limit); ++j)
                clause({-s[i - 1][j], s[i][j]});
            for (std::size_t j = 1; j < std::min(i + 1, limit); ++j)
                clause({-x[i], -s[i - 1][j - 1], s[i][j]});
        }
        return s.back();
    }

    std::vector<Literal> totalizer(const std::vector<Literal>& x,
                                   std::size_t begin, std::size_t end,
                                   std::size_t limit) {
        if (end - begin == 1) return {x[begin]};
        const auto middle = begin + (end - begin) / 2;
        auto left = totalizer(x, begin, middle, limit);
        auto right = totalizer(x, middle, end, limit);
        const auto output_size = std::min(limit, left.size() + right.size());
        std::vector<Literal> output(output_size);
        for (auto& literal : output) literal = static_cast<Literal>(variable());
        for (std::size_t i = 0; i < left.size() && i < output.size(); ++i)
            clause({-left[i], output[i]});
        for (std::size_t j = 0; j < right.size() && j < output.size(); ++j)
            clause({-right[j], output[j]});
        for (std::size_t i = 0; i < left.size(); ++i) {
            for (std::size_t j = 0; j < right.size(); ++j) {
                if (i + j + 1 >= output.size()) break;
                clause({-left[i], -right[j], output[i + j + 1]});
            }
        }
        return output;
    }

    CardinalityEncoding encoding_;
};

Literal positive(std::uint32_t variable) {
    if (variable > static_cast<std::uint32_t>(std::numeric_limits<Literal>::max()))
        throw std::overflow_error("CNF literal limit exceeded");
    return static_cast<Literal>(variable);
}

std::string encoding_name(CardinalityEncoding encoding) {
    return encoding == CardinalityEncoding::sequential_counter
        ? "sequential-counter" : "totalizer";
}

std::string fnv1a64(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

} // namespace

std::string to_dimacs(const CnfFormula& formula) {
    std::ostringstream out;
    out << "p cnf " << formula.variable_count << ' ' << formula.clauses.size() << '\n';
    for (const auto& clause : formula.clauses) {
        if (clause.empty()) {
            out << "0\n";
            continue;
        }
        for (const auto literal : clause) {
            if (literal == 0 || static_cast<std::uint32_t>(
                    literal < 0 ? -static_cast<std::int64_t>(literal) : literal) >
                    formula.variable_count)
                throw std::invalid_argument("CNF contains an invalid literal");
            out << literal << ' ';
        }
        out << "0\n";
    }
    return out.str();
}

CutwidthCnf encode_cutwidth_threshold(const Graph& graph, std::uint32_t threshold,
                                      EncodingOptions options) {
    Builder builder(options.cardinality);
    CutwidthCnf result;
    result.vertex_count = graph.size();
    result.prefix_variables.resize(graph.size());
    const auto n = graph.size();
    if (n > static_cast<std::size_t>(std::numeric_limits<Literal>::max()))
        throw std::overflow_error("graph is too large for DIMACS literals");

    for (std::size_t vertex = 0; vertex < n; ++vertex) {
        result.prefix_variables[vertex].reserve(n > 0 ? n - 1 : 0);
        for (std::size_t prefix = 1; prefix < n; ++prefix)
            result.prefix_variables[vertex].push_back(builder.variable());
    }

    if (options.channel_positions) {
        result.position_variables.assign(n, std::vector<std::uint32_t>(n));
        for (std::size_t vertex = 0; vertex < n; ++vertex)
            for (std::size_t position = 0; position < n; ++position)
                result.position_variables[vertex][position] = builder.variable();

        // A doubly stochastic Boolean permutation matrix gives exactly one
        // vertex per position and one position per vertex.
        for (std::size_t vertex = 0; vertex < n; ++vertex) {
            std::vector<Literal> row;
            row.reserve(n);
            for (std::size_t position = 0; position < n; ++position)
                row.push_back(positive(result.position_variables[vertex][position]));
            builder.exactly_one(row);
        }
        for (std::size_t position = 0; position < n; ++position) {
            std::vector<Literal> column;
            column.reserve(n);
            for (std::size_t vertex = 0; vertex < n; ++vertex)
                column.push_back(positive(result.position_variables[vertex][position]));
            builder.exactly_one(column);
        }

        // y(v,p) iff vertex v occurs at a position <= p. The recurrence is a
        // compact bidirectional channel and implies every exact-prefix count.
        if (n >= 2) {
            for (std::size_t vertex = 0; vertex < n; ++vertex) {
                const auto first_y = positive(result.prefix_variables[vertex][0]);
                const auto first_z = positive(result.position_variables[vertex][0]);
                builder.clause({-first_z, first_y});
                builder.clause({-first_y, first_z});
                for (std::size_t position = 1; position + 1 < n; ++position) {
                    const auto previous = positive(
                        result.prefix_variables[vertex][position - 1]);
                    const auto current = positive(result.prefix_variables[vertex][position]);
                    const auto placed = positive(result.position_variables[vertex][position]);
                    builder.clause({-previous, current});
                    builder.clause({-placed, current});
                    builder.clause({-current, previous, placed});
                }
                const auto last_prefix = positive(result.prefix_variables[vertex][n - 2]);
                const auto last_position = positive(result.position_variables[vertex][n - 1]);
                builder.clause({-last_position, -last_prefix});
                builder.clause({last_prefix, last_position});
            }
        }
    } else {
        // Membership is monotone in the prefix length.
        for (std::size_t vertex = 0; vertex < n; ++vertex)
            for (std::size_t prefix = 1; prefix + 1 < n; ++prefix)
                builder.clause({-positive(result.prefix_variables[vertex][prefix - 1]),
                                 positive(result.prefix_variables[vertex][prefix])});
    }

    // Select one representative from each reversal pair. If vertex 1 is in a
    // prefix then vertex 0 must already be in it, hence position(0)<position(1).
    if (options.break_reversal_symmetry && n >= 2) {
        for (std::size_t prefix = 1; prefix < n; ++prefix)
            builder.clause({-positive(result.prefix_variables[1][prefix - 1]),
                             positive(result.prefix_variables[0][prefix - 1])});
    }

    // Exactly p true membership variables at layer p: at-most p plus at-most
    // n-p negated literals. Together these also force each vertex to enter at
    // one unique position.
    for (std::size_t prefix = 1; !options.channel_positions && prefix < n; ++prefix) {
        std::vector<Literal> layer, negated;
        layer.reserve(n);
        negated.reserve(n);
        for (std::size_t vertex = 0; vertex < n; ++vertex) {
            const auto literal = positive(result.prefix_variables[vertex][prefix - 1]);
            layer.push_back(literal);
            negated.push_back(-literal);
        }
        builder.at_most(layer, prefix);
        builder.at_most(negated, n - prefix);
    }

    // At each proper prefix, y is equivalent to x_u XOR x_v. Then sum y <= K.
    result.crossing_count_outputs.resize(n > 0 ? n - 1 : 0);
    for (std::size_t prefix = 1; prefix < n; ++prefix) {
        std::vector<Literal> crossing;
        crossing.reserve(graph.edge_count());
        for (Graph::Vertex u = 0; u < n; ++u) {
            for (Graph::Vertex v = u + 1; v < n; ++v) {
                if (!graph.adjacent(u, v)) continue;
                const auto a = positive(result.prefix_variables[u][prefix - 1]);
                const auto b = positive(result.prefix_variables[v][prefix - 1]);
                const auto y = positive(builder.variable());
                builder.clause({a, b, -y});
                builder.clause({-a, -b, -y});
                builder.clause({-a, b, y});
                builder.clause({a, -b, y});
                crossing.push_back(y);
            }
        }
        if (threshold < crossing.size()) {
            const auto outputs = builder.unary_count(crossing, threshold + 1);
            builder.clause({-outputs[threshold]});
            auto& retained = result.crossing_count_outputs[prefix - 1];
            retained.reserve(outputs.size());
            for (const auto output : outputs)
                retained.push_back(static_cast<std::uint32_t>(output));
        }
    }

    result.formula = std::move(builder.formula);
    if (options.channel_positions)
        result.metadata.model = "cutwidth-position-channelled-cnf-v1";
    result.metadata.cardinality_encoding = encoding_name(options.cardinality);
    result.metadata.threshold = threshold;
    result.metadata.variables = result.formula.variable_count;
    result.metadata.clauses = result.formula.clauses.size();
    result.metadata.reversal_symmetry_breaking = options.break_reversal_symmetry;
    result.metadata.position_channeling = options.channel_positions;
    result.metadata.dimacs_fnv1a64 = fnv1a64(to_dimacs(result.formula));
    return result;
}

CutProfileCnf encode_fixed_prefix_cut_profile(
    const Graph& graph, std::span<const Graph::Mask> prefix,
    std::size_t cardinality, std::uint32_t threshold,
    CardinalityEncoding encoding) {
    if (prefix.size() != graph.word_count())
        throw std::invalid_argument("cut-profile prefix has wrong word count");
    if (!prefix.empty() && graph.size() % 64U != 0) {
        const auto used = graph.size() % 64U;
        const auto allowed = (Graph::Mask{1} << used) - 1U;
        if ((prefix.back() & ~allowed) != 0)
            throw std::invalid_argument("cut-profile prefix has high bits set");
    }

    Builder builder(encoding);
    CutProfileCnf result;
    result.cardinality = cardinality;
    for (Graph::Vertex vertex = 0; vertex < graph.size(); ++vertex) {
        if ((prefix[vertex / 64U] &
             (Graph::Mask{1} << (vertex % 64U))) == 0)
            result.residual_vertices.push_back(vertex);
    }
    if (cardinality == 0 || cardinality >= result.residual_vertices.size())
        throw std::invalid_argument(
            "cut-profile cardinality must lie strictly inside the residual set");

    std::vector<Literal> selected;
    selected.reserve(result.residual_vertices.size());
    result.selection_variables.reserve(result.residual_vertices.size());
    for (std::size_t i = 0; i < result.residual_vertices.size(); ++i) {
        const auto variable = builder.variable();
        result.selection_variables.push_back(variable);
        selected.push_back(positive(variable));
    }
    builder.at_most(selected, cardinality);
    std::vector<Literal> unselected;
    unselected.reserve(selected.size());
    for (const auto literal : selected) unselected.push_back(-literal);
    builder.at_most(unselected, selected.size() - cardinality);

    // The cut of S union T consists of residual XOR edges plus one copy of
    // !x_v for every boundary edge from v to the fixed prefix. Repeating a
    // literal is intentional: the unary counter then represents its integer
    // edge weight without introducing a pseudo-Boolean trust dependency.
    std::vector<Literal> crossing;
    crossing.reserve(graph.edge_count());
    for (std::size_t i = 0; i < result.residual_vertices.size(); ++i) {
        const auto vertex = result.residual_vertices[i];
        const auto adjacency = graph.adjacency_words(vertex);
        std::uint32_t boundary = 0;
        for (std::size_t word = 0; word < prefix.size(); ++word)
            boundary += static_cast<std::uint32_t>(
                std::popcount(adjacency[word] & prefix[word]));
        for (std::uint32_t copy = 0; copy < boundary; ++copy)
            crossing.push_back(-selected[i]);

        for (std::size_t j = i + 1; j < result.residual_vertices.size(); ++j) {
            if (!graph.adjacent(vertex, result.residual_vertices[j])) continue;
            const auto a = selected[i];
            const auto b = selected[j];
            const auto y = positive(builder.variable());
            builder.clause({a, b, -y});
            builder.clause({-a, -b, -y});
            builder.clause({-a, b, y});
            builder.clause({a, -b, y});
            crossing.push_back(y);
        }
    }
    builder.at_most(crossing, threshold);

    result.formula = std::move(builder.formula);
    result.metadata.model = "fixed-prefix-cut-profile-cnf-v1";
    result.metadata.cardinality_encoding = encoding_name(encoding);
    result.metadata.threshold = threshold;
    result.metadata.variables = result.formula.variable_count;
    result.metadata.clauses = result.formula.clauses.size();
    result.metadata.reversal_symmetry_breaking = false;
    result.metadata.position_channeling = false;
    result.metadata.dimacs_fnv1a64 = fnv1a64(to_dimacs(result.formula));
    return result;
}

std::vector<Graph::Vertex> decode_ordering(const CutwidthCnf& encoding,
                                           const std::vector<std::int8_t>& assignment) {
    const auto n = encoding.vertex_count;
    if (encoding.prefix_variables.size() != n)
        throw std::invalid_argument("CNF prefix map has wrong dimension");
    std::vector<Graph::Vertex> ordering(n, 0);
    std::vector<bool> occupied(n, false);
    for (std::size_t vertex = 0; vertex < n; ++vertex) {
        if (encoding.prefix_variables[vertex].size() != (n > 0 ? n - 1 : 0))
            throw std::invalid_argument("CNF prefix map has wrong dimension");
        std::size_t position = n == 0 ? 0 : n - 1;
        bool previously_true = false;
        for (std::size_t prefix = 0; prefix + 1 < n; ++prefix) {
            const auto variable = encoding.prefix_variables[vertex][prefix];
            if (variable >= assignment.size())
                throw std::invalid_argument("SAT assignment omits a prefix variable");
            if (assignment[variable] != 0 && assignment[variable] != 1)
                throw std::invalid_argument("SAT assignment omits a prefix variable");
            const bool member = assignment[variable] == 1;
            if (previously_true && !member)
                throw std::invalid_argument("SAT assignment has non-monotone prefixes");
            if (member && !previously_true) position = prefix;
            previously_true = member;
        }
        if (occupied[position])
            throw std::invalid_argument("SAT assignment does not project to an ordering");
        occupied[position] = true;
        ordering[position] = static_cast<Graph::Vertex>(vertex);
    }
    return ordering;
}

std::vector<Graph::Vertex> decode_cut_profile_subset(
    const CutProfileCnf& encoding, const std::vector<std::int8_t>& assignment) {
    if (encoding.selection_variables.size() != encoding.residual_vertices.size())
        throw std::invalid_argument("cut-profile selection map has wrong dimension");
    std::vector<Graph::Vertex> subset;
    subset.reserve(encoding.cardinality);
    for (std::size_t i = 0; i < encoding.selection_variables.size(); ++i) {
        const auto variable = encoding.selection_variables[i];
        if (variable >= assignment.size() ||
            (assignment[variable] != 0 && assignment[variable] != 1))
            throw std::invalid_argument("SAT assignment omits a cut-profile variable");
        if (assignment[variable] == 1)
            subset.push_back(encoding.residual_vertices[i]);
    }
    if (subset.size() != encoding.cardinality)
        throw std::invalid_argument("SAT assignment has wrong cut-profile cardinality");
    return subset;
}

bool verify_cut_profile_subset(
    const Graph& graph, std::span<const Graph::Mask> prefix,
    std::size_t cardinality, std::uint32_t threshold,
    std::span<const Graph::Vertex> subset) {
    if (prefix.size() != graph.word_count() || subset.size() != cardinality)
        return false;
    std::vector<Graph::Mask> combined(prefix.begin(), prefix.end());
    for (const auto vertex : subset) {
        if (vertex >= graph.size()) return false;
        const auto mask = Graph::Mask{1} << (vertex % 64U);
        auto& word = combined[vertex / 64U];
        if ((word & mask) != 0) return false;
        word |= mask;
    }
    return graph.cut(combined) <= threshold;
}

std::uint32_t ordering_cutwidth(const Graph& graph,
                                const std::vector<Graph::Vertex>& ordering) {
    if (!graph.validate_ordering(ordering))
        throw std::invalid_argument("ordering is not a graph vertex permutation");
    return graph.ordering_cutwidth(ordering);
}

bool verify_ordering(const Graph& graph, const std::vector<Graph::Vertex>& ordering,
                     std::uint32_t threshold) {
    return graph.validate_ordering(ordering) && graph.ordering_cutwidth(ordering) <= threshold;
}

} // namespace cutwidth::pb
