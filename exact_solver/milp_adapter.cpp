#include "milp_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

#ifdef CUTWIDTH_HAVE_HIGHS
#include <Highs.h>
#endif

namespace cutwidth {
namespace {

std::string x(std::size_t vertex, std::size_t position) {
    return "x_" + std::to_string(vertex) + '_' + std::to_string(position);
}
std::string y(std::size_t vertex, std::size_t prefix) {
    return "y_" + std::to_string(vertex) + '_' + std::to_string(prefix);
}
std::string z(std::size_t edge, std::size_t prefix) {
    return "z_" + std::to_string(edge) + '_' + std::to_string(prefix);
}

std::optional<std::pair<Graph::Vertex, Graph::Vertex>> reversal_anchors(
    const Graph& graph, const MilpModelOptions& options) {
    if (!options.reversal_symmetry || graph.size() < 2) return std::nullopt;
    const auto first = options.reversal_first_vertex;
    const auto second = options.reversal_second_vertex.value_or(
        static_cast<Graph::Vertex>(graph.size() - 1));
    if (first >= graph.size() || second >= graph.size() || first == second)
        throw std::invalid_argument("MILP reversal anchors must be distinct graph vertices");
    return std::pair{first, second};
}

} // namespace

void write_cutwidth_lp(std::ostream& out, const Graph& graph, MilpModelOptions options) {
    const std::size_t n = graph.size();
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    edges.reserve(graph.edge_count());
    for (Graph::Vertex u = 0; u < n; ++u)
        for (Graph::Vertex v = u + 1; v < n; ++v)
            if (graph.adjacent(u, v)) edges.emplace_back(u, v);

    out << "Minimize\n obj: W\nSubject To\n";
    for (std::size_t v = 0; v < n; ++v) {
        out << " assign_v" << v << ':';
        for (std::size_t p = 0; p < n; ++p) out << " + " << x(v, p);
        out << " = 1\n";
    }
    for (std::size_t p = 0; p < n; ++p) {
        out << " assign_p" << p << ':';
        for (std::size_t v = 0; v < n; ++v) out << " + " << x(v, p);
        out << " = 1\n";
    }
    for (std::size_t v = 0; v < n; ++v) {
        for (std::size_t p = 0; p + 1 < n; ++p) {
            out << " prefix_" << v << '_' << p << ": + " << y(v, p);
            for (std::size_t q = 0; q <= p; ++q) out << " - " << x(v, q);
            out << " = 0\n";
        }
    }
    for (std::size_t p = 0; p + 1 < n; ++p) {
        out << " width_" << p << ": + W";
        for (std::size_t e = 0; e < edges.size(); ++e) out << " - " << z(e, p);
        out << " >= 0\n";
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const auto [u, v] = edges[e];
            out << " xor_a_" << e << '_' << p << ": + " << z(e, p)
                << " - " << y(u, p) << " + " << y(v, p) << " >= 0\n";
            out << " xor_b_" << e << '_' << p << ": + " << z(e, p)
                << " + " << y(u, p) << " - " << y(v, p) << " >= 0\n";
        }
    }
    if (options.threshold) out << " decision_limit: + W <= " << *options.threshold << "\n";
    if (const auto anchors = reversal_anchors(graph, options)) {
        const auto [first, second] = *anchors;
        out << " reversal:";
        for (std::size_t p = 1; p < n; ++p) {
            out << " + " << p << ' ' << x(first, p)
                << " - " << p << ' ' << x(second, p);
        }
        out << " <= -1\n";
    }
    out << "Bounds\n 0 <= W\n";
    for (std::size_t v = 0; v < n; ++v)
        for (std::size_t p = 0; p + 1 < n; ++p)
            out << " 0 <= " << y(v, p) << " <= 1\n";
    for (std::size_t e = 0; e < edges.size(); ++e)
        for (std::size_t p = 0; p + 1 < n; ++p)
            out << " 0 <= " << z(e, p) << " <= 1\n";
    out << "Binary\n";
    for (std::size_t v = 0; v < n; ++v)
        for (std::size_t p = 0; p < n; ++p) out << ' ' << x(v, p) << '\n';
    out << "End\n";
    if (!out) throw std::runtime_error("failed while writing MILP model");
}

MilpResult parse_highs_output(std::string_view output, int exit_code) {
    MilpResult result;
    result.exit_code = exit_code;
    if (exit_code != 0) {
        result.status = MilpStatus::error;
        result.diagnostic = "HiGHS process did not exit successfully";
        return result;
    }
    const std::string text(output);
    // Keep this pattern portable to MSVC: unlike libstdc++, MSVC's
    // std::regex does not provide the multiline syntax option.
    const std::regex status_re(R"(Model status\s*:\s*(Optimal|Infeasible|Time limit reached|Iteration limit reached))",
                               std::regex::icase);
    std::smatch status_match;
    if (!std::regex_search(text, status_match, status_re)) {
        result.status = exit_code == 0 ? MilpStatus::unknown : MilpStatus::error;
        result.diagnostic = "HiGHS output contains no recognized model status";
        return result;
    }
    auto status = status_match[1].str();
    std::transform(status.begin(), status.end(), status.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (status == "infeasible") {
        result.status = MilpStatus::infeasible;
        return result;
    }
    if (status.find("limit") != std::string::npos) {
        result.status = MilpStatus::limit;
        return result;
    }
    const std::regex bound_re(R"(Primal bound\s+([-+0-9.eE]+))", std::regex::icase);
    std::smatch bound_match;
    if (!std::regex_search(text, bound_match, bound_re)) {
        result.status = MilpStatus::unknown;
        result.diagnostic = "optimal status has no primal bound";
        return result;
    }
    try {
        const double value = std::stod(bound_match[1].str());
        const double rounded = std::round(value);
        if (!std::isfinite(value) || std::abs(value - rounded) > 1e-6 || rounded < 0 ||
            rounded > std::numeric_limits<std::uint32_t>::max())
            throw std::out_of_range("non-integral cutwidth objective");
        result.optimum = static_cast<std::uint32_t>(rounded);
        result.status = MilpStatus::optimal;
    } catch (const std::exception&) {
        result.status = MilpStatus::unknown;
        result.diagnostic = "optimal primal bound is not a nonnegative integer";
    }
    return result;
}

MilpResult run_highs(const Graph& graph, MilpModelOptions model,
                     std::optional<double> time_limit_seconds) {
    if (time_limit_seconds &&
        (!std::isfinite(*time_limit_seconds) || *time_limit_seconds < 0))
        throw std::invalid_argument("invalid HiGHS time limit");
#ifndef CUTWIDTH_HAVE_HIGHS
    (void)graph;
    (void)model;
    return {MilpStatus::unavailable, std::nullopt, std::nullopt, -1,
            "HiGHS C++ library was not found at configure time", {}, 0.0, 0.0, 0,
            std::nullopt};
#else
    using MilpClock = std::chrono::steady_clock;
    const auto build_started = MilpClock::now();
    const std::size_t n = graph.size();
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    edges.reserve(graph.edge_count());
    for (Graph::Vertex u = 0; u < n; ++u)
        for (Graph::Vertex v = u + 1; v < n; ++v)
            if (graph.adjacent(u, v)) edges.emplace_back(u, v);
    const std::size_t prefix_count = n == 0 ? 0 : n - 1;
    const std::size_t x_count = n * n;
    const std::size_t y_count = n * prefix_count;
    const std::size_t z_count = edges.size() * prefix_count;
    const std::size_t total_columns = x_count + y_count + z_count + 1;
    if (total_columns > static_cast<std::size_t>(std::numeric_limits<HighsInt>::max()))
        throw std::length_error("MILP has too many columns for HiGHS");
    auto x_col = [n](std::size_t v, std::size_t p) { return v * n + p; };
    auto y_col = [x_count, prefix_count](std::size_t v, std::size_t p) {
        return x_count + v * prefix_count + p;
    };
    auto z_col = [x_count, y_count, prefix_count](std::size_t e, std::size_t p) {
        return x_count + y_count + e * prefix_count + p;
    };
    const std::size_t w_col = total_columns - 1;
    HighsLp lp;
    lp.num_col_ = static_cast<HighsInt>(total_columns);
    lp.col_cost_.assign(total_columns, 0.0);
    lp.col_cost_[w_col] = 1.0;
    lp.col_lower_.assign(total_columns, 0.0);
    lp.col_upper_.assign(total_columns, 1.0);
    lp.col_upper_[w_col] = kHighsInf;
    lp.integrality_.assign(total_columns, HighsVarType::kContinuous);
    std::fill_n(lp.integrality_.begin(), static_cast<std::ptrdiff_t>(x_count),
                HighsVarType::kInteger);
    lp.sense_ = ObjSense::kMinimize;
    lp.a_matrix_.format_ = MatrixFormat::kRowwise;
    // HighsSparseMatrix::clear() seeds start_ with zero. Assign explicitly to
    // avoid an extra empty first row when constructing a fresh HighsLp.
    lp.a_matrix_.start_.assign(1, 0);
    auto add_row = [&](double lower, double upper,
                       std::initializer_list<std::pair<std::size_t, double>> entries) {
        lp.row_lower_.push_back(lower);
        lp.row_upper_.push_back(upper);
        for (const auto [column, value] : entries) {
            lp.a_matrix_.index_.push_back(static_cast<HighsInt>(column));
            lp.a_matrix_.value_.push_back(value);
        }
        lp.a_matrix_.start_.push_back(static_cast<HighsInt>(lp.a_matrix_.index_.size()));
    };
    auto add_dense_row = [&](double lower, double upper,
                             const std::vector<std::pair<std::size_t, double>>& entries) {
        lp.row_lower_.push_back(lower);
        lp.row_upper_.push_back(upper);
        for (const auto [column, value] : entries) {
            lp.a_matrix_.index_.push_back(static_cast<HighsInt>(column));
            lp.a_matrix_.value_.push_back(value);
        }
        lp.a_matrix_.start_.push_back(static_cast<HighsInt>(lp.a_matrix_.index_.size()));
    };
    std::vector<std::pair<std::size_t, double>> row;
    row.reserve(std::max(n, edges.size() + 1));
    for (std::size_t v = 0; v < n; ++v) {
        row.clear();
        for (std::size_t p = 0; p < n; ++p) row.emplace_back(x_col(v, p), 1.0);
        add_dense_row(1.0, 1.0, row);
    }
    for (std::size_t p = 0; p < n; ++p) {
        row.clear();
        for (std::size_t v = 0; v < n; ++v) row.emplace_back(x_col(v, p), 1.0);
        add_dense_row(1.0, 1.0, row);
    }
    for (std::size_t v = 0; v < n; ++v) {
        for (std::size_t p = 0; p < prefix_count; ++p) {
            if (p == 0) add_row(0.0, 0.0, {{y_col(v, p), 1.0}, {x_col(v, p), -1.0}});
            else add_row(0.0, 0.0, {{y_col(v, p), 1.0},
                                              {y_col(v, p - 1), -1.0},
                                              {x_col(v, p), -1.0}});
        }
    }
    for (std::size_t p = 0; p < prefix_count; ++p) {
        row.clear();
        row.emplace_back(w_col, 1.0);
        for (std::size_t e = 0; e < edges.size(); ++e) row.emplace_back(z_col(e, p), -1.0);
        add_dense_row(0.0, kHighsInf, row);
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const auto [u, v] = edges[e];
            add_row(0.0, kHighsInf, {{z_col(e, p), 1.0}, {y_col(u, p), -1.0}, {y_col(v, p), 1.0}});
            add_row(0.0, kHighsInf, {{z_col(e, p), 1.0}, {y_col(u, p), 1.0}, {y_col(v, p), -1.0}});
        }
    }
    if (model.threshold) add_row(-kHighsInf, static_cast<double>(*model.threshold), {{w_col, 1.0}});
    if (const auto anchors = reversal_anchors(graph, model)) {
        const auto [first, second] = *anchors;
        row.clear();
        for (std::size_t p = 1; p < n; ++p) {
            row.emplace_back(x_col(first, p), static_cast<double>(p));
            row.emplace_back(x_col(second, p), -static_cast<double>(p));
        }
        add_dense_row(-kHighsInf, -1.0, row);
    }
    lp.num_row_ = static_cast<HighsInt>(lp.row_lower_.size());
    lp.a_matrix_.num_col_ = lp.num_col_;
    lp.a_matrix_.num_row_ = lp.num_row_;
    MilpResult result;
    result.model_build_seconds = std::chrono::duration<double>(MilpClock::now() - build_started).count();
    if (time_limit_seconds && result.model_build_seconds >= *time_limit_seconds) {
        result.status = MilpStatus::limit;
        result.diagnostic = "MILP time budget exhausted during model construction";
        return result;
    }
    Highs highs;
    highs.setOptionValue("output_flag", false);
    if (time_limit_seconds) {
        // MIP interrupt callbacks are periodic rather than continuous. Keep a
        // small guard interval so callback latency does not consume the
        // caller's global deadline.
        const double solve_budget = std::max(
            0.001, *time_limit_seconds - result.model_build_seconds - 0.25);
        highs.setOptionValue("time_limit", solve_budget);
        // HiGHS presolve can substantially overrun short MIP budgets before
        // checking its timer. Disable it for explicitly bounded oracle calls;
        // exactness is unaffected and the MIP loop honors interrupts promptly.
        highs.setOptionValue("presolve", "off");
        const auto deadline = MilpClock::now() + std::chrono::duration_cast<MilpClock::duration>(
            std::chrono::duration<double>(solve_budget));
        highs.setCallback([deadline](int, const std::string&,
                                     const HighsCallbackOutput*,
                                     HighsCallbackInput* input, void*) {
            if (MilpClock::now() >= deadline) input->user_interrupt = true;
        });
        highs.startCallback(HighsCallbackType::kCallbackMipInterrupt);
    }
    const auto pass_status = highs.passModel(std::move(lp));
    if (pass_status != HighsStatus::kOk) {
        result.status = MilpStatus::error;
        result.diagnostic = "HiGHS rejected direct sparse model";
    } else {
      const auto solve_started = MilpClock::now();
      const auto run_status = highs.run();
      result.solve_seconds = std::chrono::duration<double>(MilpClock::now() - solve_started).count();
      const auto& info = highs.getInfo();
      result.mip_nodes = info.mip_node_count;
      if (std::isfinite(info.mip_dual_bound)) result.diagnostic_dual_bound = info.mip_dual_bound;
      if (run_status == HighsStatus::kError) {
        result.status = MilpStatus::error;
        result.diagnostic = "HiGHS failed while solving model";
      } else {
        switch (highs.getModelStatus()) {
        case HighsModelStatus::kOptimal: {
            const double value = highs.getObjectiveValue();
            const double rounded = std::round(value);
            if (std::isfinite(value) && std::abs(value - rounded) <= 1e-6 &&
                rounded >= 0 && rounded <= std::numeric_limits<std::uint32_t>::max()) {
                result.status = MilpStatus::optimal;
                result.optimum = static_cast<std::uint32_t>(rounded);
            } else {
                result.status = MilpStatus::unknown;
                result.diagnostic = "HiGHS returned a non-integral objective";
            }
            break;
        }
        case HighsModelStatus::kInfeasible:
            result.status = MilpStatus::infeasible;
            break;
        case HighsModelStatus::kTimeLimit:
        case HighsModelStatus::kIterationLimit:
        case HighsModelStatus::kInterrupt:
            result.status = MilpStatus::limit;
            break;
        default:
            result.status = MilpStatus::unknown;
            result.diagnostic = "HiGHS did not return a certifying model status";
            break;
        }
        if (result.status == MilpStatus::optimal || result.status == MilpStatus::limit) {
            const auto& solution = highs.getSolution();
            result.ordering.assign(graph.size(), 0);
            std::vector<bool> assigned(graph.size(), false);
            if (solution.value_valid) {
                for (std::size_t vertex = 0; vertex < n; ++vertex) {
                    for (std::size_t position = 0; position < n; ++position) {
                        if (solution.col_value[x_col(vertex, position)] < 0.5) continue;
                        if (assigned[position]) break;
                        result.ordering[position] = static_cast<Graph::Vertex>(vertex);
                        assigned[position] = true;
                    }
                }
            }
            const bool valid = std::all_of(assigned.begin(), assigned.end(), [](bool value) { return value; }) &&
                graph.validate_ordering(result.ordering);
            if (valid) result.incumbent_width = graph.ordering_cutwidth(result.ordering);
            else result.ordering.clear();
            if (result.status == MilpStatus::optimal &&
                (!result.incumbent_width || result.incumbent_width != result.optimum)) {
                result.status = MilpStatus::unknown;
                result.optimum.reset();
                result.incumbent_width.reset();
                result.ordering.clear();
                result.diagnostic = "HiGHS witness failed independent verification";
            }
        }
      }
    }
    return result;
#endif
}

} // namespace cutwidth
