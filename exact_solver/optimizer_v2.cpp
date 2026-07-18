#include "optimizer_v2.hpp"

#include "decomposition.hpp"
#include "decision_session.hpp"
#include "incumbent_session.hpp"
#include "incremental_layout.hpp"
#include "threshold_portfolio.hpp"
#include "residual_dp.hpp"
#include "parallel_decision_session.hpp"
#include "parallel_global_dfs_session.hpp"
#include "global_dfs_executor.hpp"
#include "progressive_sdp_session.hpp"
#include "progressive_cheap_bound_session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <stdexcept>

#ifdef CUTWIDTH_ENABLE_SDP_PROTOTYPE
#include "sdp_admm.hpp"
#include "sdp_certificate.hpp"
#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
#include "clarabel_sdp_adapter.hpp"
#endif
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cutwidth {
namespace {

enum class ParsedStatus { unknown, sat, unsat };

ParsedStatus parse_status(const std::string& output, int exit_code) {
    if (exit_code == 10) return ParsedStatus::sat;
    if (exit_code == 20) return ParsedStatus::unsat;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line == "s SATISFIABLE" || line == "SAT") return ParsedStatus::sat;
        if (line == "s UNSATISFIABLE" || line == "UNSAT") return ParsedStatus::unsat;
    }
    return ParsedStatus::unknown;
}

struct ProcessResult {
    bool launched = false;
    bool timed_out = false;
    int exit_code = -1;
};

#if defined(__unix__) || defined(__APPLE__)
ProcessResult run_process_posix(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& log_path,
    std::chrono::milliseconds timeout,
    std::stop_token stop_token)
{
    ProcessResult result;
    int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log_fd < 0) return result;

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : arguments) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(log_fd);
        return result;
    }
    if (::posix_spawnattr_init(&attributes) != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(log_fd);
        return result;
    }

    ::posix_spawn_file_actions_adddup2(&actions, log_fd, STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, log_fd);
    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    ::posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid = -1;
    int spawn_err = ::posix_spawn(
        &pid, executable.c_str(), &actions, &attributes, argv.data(), ::environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attributes);
    ::close(log_fd);

    if (spawn_err != 0 || pid <= 0) {
        return result;
    }

    result.launched = true;
    auto start_time = std::chrono::steady_clock::now();
    int status = 0;
    while (true) {
        pid_t wait_res = ::waitpid(pid, &status, WNOHANG);
        if (wait_res == pid) {
            break;
        }
        if (wait_res < 0 && errno != EINTR) {
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        if (stop_token.stop_requested() || (timeout.count() > 0 && elapsed >= timeout)) {
            if (elapsed >= timeout) {
                result.timed_out = true;
            }
            ::kill(-pid, SIGTERM);
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                if (::waitpid(pid, &status, WNOHANG) == pid) {
                    goto reaped;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

reaped:
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}
#endif

#ifdef _WIN32
std::wstring quote_windows_argument(const std::wstring& argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring quoted{L'\"'};
    std::size_t backslashes = 0;
    for (const auto ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

ProcessResult run_process_windows(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& log_path,
    std::chrono::milliseconds timeout,
    std::stop_token stop_token)
{
    ProcessResult result;
    const auto executable_wide = std::filesystem::path(executable).wstring();
    std::wstring cmd_line = quote_windows_argument(executable_wide);
    for (const auto& arg : arguments) {
        cmd_line.push_back(L' ');
        cmd_line += quote_windows_argument(std::filesystem::path(arg).wstring());
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hLogFile = CreateFileW(
        log_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hLogFile == INVALID_HANDLE_VALUE) {
        return result;
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hLogFile;
    si.hStdError = hLogFile;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmd_line_chars(cmd_line.begin(), cmd_line.end());
    cmd_line_chars.push_back(L'\0');

    BOOL success = CreateProcessW(
        executable_wide.c_str(),
        cmd_line_chars.data(),
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );

    CloseHandle(hLogFile);

    if (!success) {
        return result;
    }

    result.launched = true;
    auto start_time = std::chrono::steady_clock::now();
    DWORD exit_code = 0;

    while (true) {
        DWORD wait_res = WaitForSingleObject(pi.hProcess, 10);
        if (wait_res == WAIT_OBJECT_0) {
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        if (stop_token.stop_requested() || (timeout.count() > 0 && elapsed >= timeout)) {
            if (elapsed >= timeout) {
                result.timed_out = true;
            }
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            break;
        }
    }

    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        result.exit_code = static_cast<int>(exit_code);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}
#endif

ProcessResult run_process_portable(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& log_path,
    std::chrono::milliseconds timeout,
    std::stop_token stop_token)
{
#ifdef _WIN32
    return run_process_windows(executable, arguments, log_path, timeout, stop_token);
#else
    return run_process_posix(executable, arguments, log_path, timeout, stop_token);
#endif
}

struct PbSatRootJobState {
    enum class Status { idle, solving, checking, certified_unsat, sat, timed_out, failed };
    std::atomic<Status> status{Status::idle};
    std::uint32_t threshold = 0;
    std::size_t cardinality = 0;
    std::string cnf_path;
    std::string proof_path;
    std::string solver_log_path;
    std::string checker_log_path;
    double solver_seconds = 0.0;
    double checker_seconds = 0.0;
};

void run_pb_sat_root_job(
    std::stop_token stop_token,
    const Graph& graph,
    std::string solver_path,
    std::string checker_path,
    std::string dir_path,
    std::chrono::milliseconds timeout,
    std::size_t q,
    std::uint32_t K,
    std::shared_ptr<PbSatRootJobState> state)
{
    try {
        state->status.store(PbSatRootJobState::Status::solving, std::memory_order_release);
        state->threshold = K;
        state->cardinality = q;

        std::filesystem::path base_dir(dir_path);
        std::error_code ec;
        std::filesystem::create_directories(base_dir, ec);
        if (ec) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        std::string prefix = "root_q" + std::to_string(q) + "_K" + std::to_string(K);
        state->cnf_path = (base_dir / (prefix + ".cnf")).string();
        state->proof_path = (base_dir / (prefix + ".drat")).string();
        state->solver_log_path = (base_dir / (prefix + "_solver.log")).string();
        state->checker_log_path = (base_dir / (prefix + "_checker.log")).string();

        std::filesystem::remove(state->proof_path, ec);

        if (stop_token.stop_requested()) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        std::vector<Graph::Mask> empty_prefix(graph.word_count(), 0);
        auto encoded = cutwidth::pb::encode_fixed_prefix_cut_profile(
            graph, empty_prefix, q, K, cutwidth::pb::CardinalityEncoding::totalizer);

        std::ofstream output(state->cnf_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }
        output << cutwidth::pb::to_dimacs(encoded.formula);
        output.close();
        if (!output) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        if (stop_token.stop_requested()) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        auto solver_start = std::chrono::steady_clock::now();
        ProcessResult solver_res = run_process_portable(
            solver_path,
            { "-q", state->cnf_path, state->proof_path },
            state->solver_log_path,
            timeout,
            stop_token
        );
        auto solver_end = std::chrono::steady_clock::now();
        state->solver_seconds = std::chrono::duration<double>(solver_end - solver_start).count();

        if (stop_token.stop_requested()) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        if (solver_res.timed_out) {
            state->status.store(PbSatRootJobState::Status::timed_out, std::memory_order_release);
            return;
        }
        if (!solver_res.launched) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        std::string solver_output;
        {
            std::ifstream log_file(state->solver_log_path, std::ios::binary);
            if (log_file) {
                std::ostringstream ss;
                ss << log_file.rdbuf();
                solver_output = ss.str();
            }
        }

        ParsedStatus parsed = parse_status(solver_output, solver_res.exit_code);
        if (parsed == ParsedStatus::sat) {
            state->status.store(PbSatRootJobState::Status::sat, std::memory_order_release);
            return;
        }
        if (parsed != ParsedStatus::unsat) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        std::error_code proof_ec;
        const auto proof_size = std::filesystem::file_size(state->proof_path, proof_ec);
        if (proof_ec || proof_size == 0) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        state->status.store(PbSatRootJobState::Status::checking, std::memory_order_release);
        auto checker_start = std::chrono::steady_clock::now();
        ProcessResult checker_res = run_process_portable(
            checker_path,
            { state->cnf_path, state->proof_path },
            state->checker_log_path,
            timeout,
            stop_token
        );
        auto checker_end = std::chrono::steady_clock::now();
        state->checker_seconds = std::chrono::duration<double>(checker_end - checker_start).count();

        if (stop_token.stop_requested()) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        if (checker_res.timed_out) {
            state->status.store(PbSatRootJobState::Status::timed_out, std::memory_order_release);
            return;
        }
        if (!checker_res.launched) {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
            return;
        }

        if (checker_res.exit_code == 0) {
            state->status.store(PbSatRootJobState::Status::certified_unsat, std::memory_order_release);
        } else {
            state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
        }
    } catch (...) {
        state->status.store(PbSatRootJobState::Status::failed, std::memory_order_release);
    }
}



using Clock = std::chrono::steady_clock;
using Vertex = Graph::Vertex;
bool deadline_reached(Clock::time_point start, std::chrono::milliseconds limit);
std::optional<std::vector<Vertex>> greedy_order(
    const Graph& graph, Vertex first, Clock::time_point start,
    std::chrono::milliseconds limit);

std::string_view yield_reason_name(SessionYieldReason reason) {
    switch (reason) {
    case SessionYieldReason::quantum_complete: return "quantum_complete";
    case SessionYieldReason::yield_requested: return "yield_requested";
    case SessionYieldReason::worker_donation: return "worker_donation";
    case SessionYieldReason::ownership_wait: return "ownership_wait";
    case SessionYieldReason::memory_pressure: return "memory_pressure";
    case SessionYieldReason::interval_resolved: return "interval_resolved";
    case SessionYieldReason::deadline: return "deadline";
    case SessionYieldReason::exception: return "exception";
    case SessionYieldReason::terminal: return "terminal";
    }
    return "unknown";
}

// A one-lease controller arm.  It deliberately has no background thread: the
// global exact-search executor owns both admission and execution, so a
// controller window can account DFS and a bounded auxiliary service against
// one fixed worker budget.
class OneShotGlobalWorkSession final : public GlobalWorkSession {
public:
    explicit OneShotGlobalWorkSession(
        std::function<void(Clock::time_point)> work) : work_(std::move(work)) {}
    void prepare(std::uint64_t generation, std::uint32_t threshold) override {
        generation_ = generation;
        threshold_ = threshold;
        ready_ = true;
    }
    LeaseOutcome run_one_lease(std::size_t, Clock::time_point deadline) override {
        LeaseOutcome outcome;
        if (!ready_ || revoked_) {
            outcome.status = LeaseOutcome::empty;
            return outcome;
        }
        const auto started = Clock::now();
        try { work_(deadline); }
        catch (...) { error_ = std::current_exception(); }
        busy_ += std::chrono::duration<double>(Clock::now() - started).count();
        ready_ = false;
        outcome.status = LeaseOutcome::useful;
        outcome.consumed_work_units = 1;
        outcome.nodes_expanded = 1;
        outcome.busy_seconds = std::chrono::duration<double>(
            Clock::now() - started).count();
        return outcome;
    }
    void quiesce() override {}
    bool has_work() const override { return ready_ && !revoked_; }
    bool has_runnable_work() const override { return has_work(); }
    std::uint32_t threshold() const override { return threshold_; }
    std::uint64_t generation() const override { return generation_; }
    void revoke() override { revoked_ = true; }
    bool is_revoked() const override { return revoked_; }
    double busy_worker_seconds() const override { return busy_; }
    double allocated_worker_seconds() const override { return busy_; }
    void rethrow_if_error() const { if (error_) std::rethrow_exception(error_); }
private:
    std::function<void(Clock::time_point)> work_;
    std::uint64_t generation_ = 0;
    std::uint32_t threshold_ = 0;
    bool ready_ = false, revoked_ = false;
    double busy_ = 0.0;
    std::exception_ptr error_;
};

std::pair<std::uint32_t, std::vector<Vertex>> incremental_heuristic(
    const Graph& graph, Clock::time_point start, std::chrono::milliseconds limit,
    const OptimizerV2Options& policy, OptimizerV2Stats* stats) {
    if (graph.size() == 0) return {0, {}};
    std::vector<Vertex> best(graph.size());
    std::iota(best.begin(), best.end(), Vertex{0});
    auto best_width = graph.ordering_cutwidth(best);
    auto best_profile = IncrementalLayoutEvaluator(graph, best).descending_cut_profile();
    auto note_best = [&] {
        if (stats) stats->time_to_final_upper_bound_seconds =
            std::chrono::duration<double>(Clock::now() - start).count();
    };
    note_best();
    for (Vertex first = 0; first < graph.size() && !deadline_reached(start, limit); ++first) {
        auto seed = greedy_order(graph, first, start, limit);
        if (!seed) break;
        IncrementalLayoutEvaluator eval(graph, std::move(*seed));
        bool changed = true;
        while (changed && !deadline_reached(start, limit)) {
            changed = false;
            for (std::size_t i = 0; i + 1 < graph.size(); ++i) {
                const auto cuts = eval.cuts_after_swap(i, i + 1);
                const auto width = *std::max_element(cuts.begin(), cuts.end());
                if (width < eval.width() ||
                    (width == eval.width() && policy.heuristic_tiebreak == HeuristicTiebreak::cut_profile &&
                     cut_profile_better(cuts, eval.cuts()))) {
                    eval.apply_swap(i, i + 1); changed = true;
                }
            }
        }
        const auto profile = eval.descending_cut_profile();
        if (eval.width() < best_width ||
            (eval.width() == best_width && policy.heuristic_tiebreak == HeuristicTiebreak::cut_profile &&
             profile < best_profile)) {
            best_width = eval.width(); best = eval.ordering(); best_profile = profile; note_best();
        }
        if (stats) stats->heuristic_interval_evaluations += eval.stats().interval_evaluations;
    }
    bool improved = true;
    while (improved && !deadline_reached(start, limit)) {
        improved = false;
        IncrementalLayoutEvaluator eval(graph, best);
        std::vector<std::size_t> roi;
        for (std::size_t i = 0; i < eval.cuts().size(); ++i)
            if (eval.cuts()[i] + 1 >= eval.width()) roi.push_back(i);
        std::vector<bool> tested(graph.size() * graph.size(), false);
        auto scan = [&](bool roi_only) {
            for (std::size_t a = 0; a < graph.size() && !improved; ++a)
                for (std::size_t b = 0; b < graph.size() && !improved; ++b) {
                    if (a == b || tested[a * graph.size() + b]) continue;
                    const auto touches = std::any_of(roi.begin(), roi.end(), [&](std::size_t cut) {
                        const auto lo = std::min(a, b), hi = std::max(a, b);
                        return cut >= lo && cut <= hi;
                    });
                    if (roi_only != touches) continue;
                    tested[a * graph.size() + b] = true;
                    const auto cuts = eval.cuts_after_insertion(a, b);
                    const auto width = *std::max_element(cuts.begin(), cuts.end());
                    if (width < eval.width() ||
                        (width == eval.width() && policy.heuristic_tiebreak == HeuristicTiebreak::cut_profile &&
                         cut_profile_better(cuts, eval.cuts()))) {
                        eval.apply_insertion(a, b); improved = true;
                    }
                }
        };
        scan(true);
        if (!improved) { if (stats) ++stats->heuristic_full_fallbacks; scan(false); }
        if (improved) {
            best = eval.ordering(); best_width = eval.width();
            best_profile = eval.descending_cut_profile(); note_best();
        }
        if (stats) stats->heuristic_interval_evaluations += eval.stats().interval_evaluations;
    }
    // Final certified recomputation is intentionally independent of evaluator state.
    best_width = graph.ordering_cutwidth(best);
    return {best_width, std::move(best)};
}

bool deadline_reached(Clock::time_point start, std::chrono::milliseconds limit) {
    return limit.count() != 0 && Clock::now() - start >= limit;
}
std::optional<std::vector<Vertex>> greedy_order(
    const Graph& graph, Vertex first, Clock::time_point start,
    std::chrono::milliseconds limit) {
    const auto n = graph.size();
    std::vector<Vertex> order;
    order.reserve(n);
    std::vector<bool> placed(n, false);
    std::vector<std::uint32_t> neighbors_in_prefix(n, 0);
    std::uint32_t cut = 0;
    if (n != 0) {
        order.push_back(first);
        placed[first] = true;
        cut = graph.degree(first);
        for (Vertex other = 0; other < n; ++other)
            if (!placed[other] && graph.adjacent(first, other)) ++neighbors_in_prefix[other];
    }
    while (order.size() < n) {
        if (deadline_reached(start, limit)) return std::nullopt;
        Vertex best = 0;
        std::uint32_t best_cut = std::numeric_limits<std::uint32_t>::max();
        for (Vertex v = 0; v < n; ++v) {
            if (deadline_reached(start, limit)) return std::nullopt;
            if (placed[v]) continue;
            const auto before = neighbors_in_prefix[v];
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(cut) + graph.degree(v) - 2 * before);
            if (next < best_cut || (next == best_cut && v < best)) {
                best = v;
                best_cut = next;
            }
        }
        order.push_back(best);
        placed[best] = true;
        for (Vertex other = 0; other < n; ++other)
            if (!placed[other] && graph.adjacent(best, other)) ++neighbors_in_prefix[other];
        cut = best_cut;
    }
    return order;
}

std::pair<std::uint32_t, std::vector<Vertex>> basic_heuristic(
    const Graph& graph, Clock::time_point start, std::chrono::milliseconds limit,
    const OptimizerV2Options& policy, OptimizerV2Stats* stats = nullptr) {
    const auto heuristic_started = Clock::now();
    if (policy.heuristic_evaluation == HeuristicEvaluation::incremental) {
        auto result = incremental_heuristic(graph, start, limit, policy, stats);
        if (stats) stats->heuristic_runtime_seconds +=
            std::chrono::duration<double>(Clock::now() - heuristic_started).count();
        return result;
    }
    if (graph.size() == 0) return {0, {}};
    std::vector<Vertex> best(graph.size());
    std::iota(best.begin(), best.end(), Vertex{0});
    std::uint32_t best_width = graph.ordering_cutwidth(best);
    for (Vertex first = 0; first < graph.size(); ++first) {
        if (deadline_reached(start, limit)) break;
        auto greedy = greedy_order(graph, first, start, limit);
        if (!greedy) break;
        auto order = std::move(*greedy);
        auto width = graph.ordering_cutwidth(order);
        bool improved = true;
        while (improved && !deadline_reached(start, limit)) {
            improved = false;
            for (std::size_t i = 0; i + 1 < order.size(); ++i) {
                if (deadline_reached(start, limit)) break;
                std::swap(order[i], order[i + 1]);
                const auto candidate = graph.ordering_cutwidth(order);
                if (candidate < width) {
                    width = candidate;
                    improved = true;
                } else {
                    std::swap(order[i], order[i + 1]);
                }
            }
        }
        if (width < best_width || (width == best_width && order < best)) {
            best_width = width;
            best = std::move(order);
        }
    }
    // One-vertex relocation explores a substantially richer neighborhood than
    // adjacent swaps. Run it only on the best multi-start result to keep the
    // construction cost predictable.
    bool relocated = true;
    while (relocated && !deadline_reached(start, limit)) {
        relocated = false;
        std::vector<Vertex> improved = best;
        auto improved_width = best_width;
        for (std::size_t from = 0; from < best.size(); ++from) {
            if (deadline_reached(start, limit)) break;
            for (std::size_t to = 0; to < best.size(); ++to) {
                if (deadline_reached(start, limit)) break;
                if (from == to) continue;
                auto candidate = best;
                const auto vertex = candidate[from];
                candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(from));
                candidate.insert(candidate.begin() + static_cast<std::ptrdiff_t>(to), vertex);
                const auto width = graph.ordering_cutwidth(candidate);
                if (width < improved_width || (width == improved_width && candidate < improved)) {
                    improved_width = width;
                    improved = std::move(candidate);
                }
            }
        }
        for (std::size_t left = 0; left < best.size(); ++left) {
            if (deadline_reached(start, limit)) break;
            for (std::size_t right = left + 1; right < best.size(); ++right) {
                if (deadline_reached(start, limit)) break;
                auto candidate = best;
                std::swap(candidate[left], candidate[right]);
                const auto width = graph.ordering_cutwidth(candidate);
                if (width < improved_width || (width == improved_width && candidate < improved)) {
                    improved_width = width;
                    improved = std::move(candidate);
                }
            }
        }
        if (improved_width < best_width) {
            best_width = improved_width;
            best = std::move(improved);
            relocated = true;
        }
    }
    // Deterministic annealing over swaps and short segment reversals permits
    // temporary plateau/worsening moves that steepest descent cannot cross.
    // Work is explicitly capped and all iterations honor the global deadline.
    if (graph.size() < policy.annealing_min_vertices) {
        best_width = graph.ordering_cutwidth(best);
        if (stats) {
            stats->heuristic_runtime_seconds +=
                std::chrono::duration<double>(Clock::now() - heuristic_started).count();
            stats->time_to_final_upper_bound_seconds =
                std::chrono::duration<double>(Clock::now() - start).count();
        }
        return {best_width, std::move(best)};
    }
    auto current = best;
    auto current_width = best_width;
    std::uint64_t rng = 0x9e3779b97f4a7c15ULL ^ graph.edge_count();
    auto next_random = [&rng]() {
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        return rng * 2685821657736338717ULL;
    };
    const std::size_t scaled_iterations = graph.size() != 0 &&
            policy.annealing_iterations_per_vertex >
                std::numeric_limits<std::size_t>::max() / graph.size()
        ? std::numeric_limits<std::size_t>::max()
        : policy.annealing_iterations_per_vertex * graph.size();
    const std::size_t iterations = std::min(
        policy.annealing_max_iterations, scaled_iterations);
    for (std::size_t iteration = 0;
         iteration < iterations && !deadline_reached(start, limit); ++iteration) {
        const auto left = static_cast<std::size_t>(next_random() % best.size());
        const auto right = static_cast<std::size_t>(next_random() % best.size());
        if (left == right) continue;
        auto candidate = current;
        if ((next_random() & 3U) == 0) {
            auto a = std::min(left, right);
            auto b = std::max(left, right);
            if (b - a > 6) b = a + 6;
            std::reverse(candidate.begin() + static_cast<std::ptrdiff_t>(a),
                         candidate.begin() + static_cast<std::ptrdiff_t>(b + 1));
        } else std::swap(candidate[left], candidate[right]);
        const auto candidate_width = graph.ordering_cutwidth(candidate);
        const auto phase = (4 * iteration) / std::max<std::size_t>(1, iterations);
        const std::uint32_t temperature = static_cast<std::uint32_t>(4 - phase);
        const auto delta = candidate_width > current_width
            ? candidate_width - current_width : 0U;
        const bool accept = candidate_width <= current_width ||
            (delta <= temperature && (next_random() & 15U) < (temperature - delta + 1));
        if (accept) {
            current = std::move(candidate);
            current_width = candidate_width;
            if (current_width < best_width ||
                (current_width == best_width && current < best)) {
                best_width = current_width;
                best = current;
            }
        }
        // Periodic restart prevents an unlucky deterministic walk from spending
        // its full budget far from the incumbent basin.
        if ((iteration + 1) % (10 * graph.size()) == 0) {
            current = best;
            current_width = best_width;
        }
    }
    // Never let heuristic bookkeeping change the verified upper bound.
    best_width = graph.ordering_cutwidth(best);
    if (stats) {
        stats->heuristic_runtime_seconds +=
            std::chrono::duration<double>(Clock::now() - heuristic_started).count();
        stats->time_to_final_upper_bound_seconds =
            std::chrono::duration<double>(Clock::now() - start).count();
    }
    return {best_width, std::move(best)};
}

std::optional<std::vector<Vertex>> fiedler_ordering(
    const Graph& graph, bool reverse, Clock::time_point deadline) {
    const std::size_t n = graph.size();
    if (n < 2) {
        std::vector<Vertex> order(n);
        std::iota(order.begin(), order.end(), Vertex{0});
        return order;
    }
    std::vector<double> matrix(n * n, 0.0), eigenvectors(n * n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        matrix[i * n + i] = graph.degree(static_cast<Vertex>(i));
        eigenvectors[i * n + i] = 1.0;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (!graph.adjacent(static_cast<Vertex>(i), static_cast<Vertex>(j))) continue;
            matrix[i * n + j] = matrix[j * n + i] = -1.0;
        }
    }
    // Cyclic Jacobi diagonalization is dependency-free and deterministic.  It
    // is tiny compared with even a one-second portfolio at the target sizes.
    for (std::size_t sweep = 0; sweep < 64; ++sweep) {
        if (Clock::now() >= deadline) return std::nullopt;
        double maximum = 0.0;
        for (std::size_t p = 0; p < n; ++p) {
            for (std::size_t q = p + 1; q < n; ++q) {
                const double apq = matrix[p * n + q];
                maximum = std::max(maximum, std::abs(apq));
                if (std::abs(apq) <= 1e-13) continue;
                const double tau = (matrix[q * n + q] - matrix[p * n + p]) /
                    (2.0 * apq);
                const double t = std::copysign(1.0, tau) /
                    (std::abs(tau) + std::sqrt(1.0 + tau * tau));
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = t * c;
                const double app = matrix[p * n + p];
                const double aqq = matrix[q * n + q];
                matrix[p * n + p] = app - t * apq;
                matrix[q * n + q] = aqq + t * apq;
                matrix[p * n + q] = matrix[q * n + p] = 0.0;
                for (std::size_t k = 0; k < n; ++k) {
                    if (k != p && k != q) {
                        const double akp = matrix[k * n + p];
                        const double akq = matrix[k * n + q];
                        matrix[k * n + p] = matrix[p * n + k] = c * akp - s * akq;
                        matrix[k * n + q] = matrix[q * n + k] = s * akp + c * akq;
                    }
                    const double vkp = eigenvectors[k * n + p];
                    const double vkq = eigenvectors[k * n + q];
                    eigenvectors[k * n + p] = c * vkp - s * vkq;
                    eigenvectors[k * n + q] = s * vkp + c * vkq;
                }
            }
        }
        if (maximum <= 1e-10) break;
    }
    std::vector<std::size_t> columns(n);
    std::iota(columns.begin(), columns.end(), std::size_t{0});
    std::stable_sort(columns.begin(), columns.end(), [&](std::size_t a, std::size_t b) {
        return matrix[a * n + a] < matrix[b * n + b];
    });
    const auto fiedler = columns[1];
    std::vector<Vertex> order(n);
    std::iota(order.begin(), order.end(), Vertex{0});
    std::stable_sort(order.begin(), order.end(), [&](Vertex a, Vertex b) {
        const double x = eigenvectors[static_cast<std::size_t>(a) * n + fiedler];
        const double y = eigenvectors[static_cast<std::size_t>(b) * n + fiedler];
        return x < y || (x == y && a < b);
    });
    if (reverse) std::reverse(order.begin(), order.end());
    return order;
}

std::vector<Vertex> grasp_ordering(const Graph& graph, std::uint64_t& rng) {
    const auto next_random = [&rng]() {
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        return rng * 2685821657736338717ULL;
    };
    const std::size_t n = graph.size();
    std::vector<Vertex> order;
    order.reserve(n);
    std::vector<bool> placed(n, false);
    std::vector<std::uint32_t> before(n, 0);
    std::uint32_t cut = 0;
    while (order.size() < n) {
        struct Candidate { Vertex vertex; std::uint32_t cut; std::uint32_t degree; };
        std::vector<Candidate> candidates;
        candidates.reserve(n - order.size());
        for (Vertex v = 0; v < n; ++v) {
            if (placed[v]) continue;
            const auto next = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(cut) + graph.degree(v) - 2 * before[v]);
            candidates.push_back({v, next, graph.degree(v)});
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            if (a.cut != b.cut) return a.cut < b.cut;
            if (a.degree != b.degree) return a.degree > b.degree;
            return a.vertex < b.vertex;
        });
        // A small randomized restricted-candidate list supplies diversification
        // without discarding the strong immediate-cut construction signal.
        const std::size_t rcl = std::min<std::size_t>(
            candidates.size(), 2 + static_cast<std::size_t>(next_random() % 4));
        const auto chosen = candidates[static_cast<std::size_t>(next_random() % rcl)];
        order.push_back(chosen.vertex);
        placed[chosen.vertex] = true;
        cut = chosen.cut;
        for (Vertex v = 0; v < n; ++v)
            if (!placed[v] && graph.adjacent(chosen.vertex, v)) ++before[v];
    }
    return order;
}

std::vector<Vertex> vns_improve(
    const Graph& graph, std::vector<Vertex> seed, Clock::time_point deadline,
    OptimizerV2Stats* stats) {
    IncrementalLayoutEvaluator eval(graph, std::move(seed));
    constexpr std::size_t maximum_passes = 16;
    for (std::size_t pass = 0; pass < maximum_passes && Clock::now() < deadline; ++pass) {
        enum class Move { none, insertion, swap, reversal };
        Move best_move = Move::none;
        std::size_t best_a = 0, best_b = 0;
        auto best_cuts = eval.cuts();
        auto consider = [&](Move move, std::size_t a, std::size_t b,
                            std::vector<std::uint32_t> cuts) {
            if (stats) ++stats->heuristic_vns_evaluations;
            const auto width = *std::max_element(cuts.begin(), cuts.end());
            const auto best_width = *std::max_element(best_cuts.begin(), best_cuts.end());
            if (width < best_width ||
                (width == best_width && cut_profile_better(cuts, best_cuts))) {
                best_move = move;
                best_a = a;
                best_b = b;
                best_cuts = std::move(cuts);
            }
        };
        for (std::size_t a = 0; a < graph.size() && Clock::now() < deadline; ++a) {
            for (std::size_t b = 0; b < graph.size() && Clock::now() < deadline; ++b) {
                if (a == b) continue;
                consider(Move::insertion, a, b, eval.cuts_after_insertion(a, b));
                if (a < b) {
                    consider(Move::swap, a, b, eval.cuts_after_swap(a, b));
                    if (b - a <= 10)
                        consider(Move::reversal, a, b, eval.cuts_after_reversal(a, b));
                }
            }
        }
        if (best_move == Move::none) break;
        if (best_move == Move::insertion) eval.apply_insertion(best_a, best_b);
        else if (best_move == Move::swap) eval.apply_swap(best_a, best_b);
        else eval.apply_reversal(best_a, best_b);
    }
    if (stats) stats->heuristic_interval_evaluations += eval.stats().interval_evaluations;
    return eval.ordering();
}

std::pair<std::uint32_t, std::vector<Vertex>> heuristic(
    const Graph& graph, Clock::time_point start, std::chrono::milliseconds global_limit,
    const OptimizerV2Options& policy, OptimizerV2Stats* stats = nullptr) {
    auto effective_limit = global_limit;
    if (policy.heuristic_search == HeuristicSearch::portfolio &&
        policy.heuristic_time.count() > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start);
        const auto portfolio_limit = elapsed + policy.heuristic_time;
        if (effective_limit.count() == 0 || portfolio_limit < effective_limit)
            effective_limit = portfolio_limit;
    }
    auto incumbent = basic_heuristic(graph, start, effective_limit, policy, stats);
    if (policy.heuristic_search != HeuristicSearch::portfolio || graph.size() < 3 ||
        policy.heuristic_time.count() == 0 || deadline_reached(start, effective_limit))
        return incumbent;

    const auto portfolio_started = Clock::now();
    const auto deadline = start + effective_limit;
    auto accept = [&](std::vector<Vertex> candidate) {
        const auto width = graph.ordering_cutwidth(candidate);
        if (width < incumbent.first || (width == incumbent.first && candidate < incumbent.second)) {
            if (width < incumbent.first && stats) ++stats->heuristic_portfolio_improvements;
            incumbent = {width, std::move(candidate)};
            if (stats) stats->time_to_final_upper_bound_seconds =
                std::chrono::duration<double>(Clock::now() - start).count();
        }
    };
    for (const bool reverse : {false, true}) {
        auto spectral = fiedler_ordering(graph, reverse, deadline);
        if (!spectral) break;
        if (stats) ++stats->heuristic_spectral_seeds;
        accept(vns_improve(graph, std::move(*spectral), deadline, stats));
    }
    std::uint64_t rng = 0x6a09e667f3bcc909ULL ^
        (static_cast<std::uint64_t>(graph.size()) << 32) ^ graph.edge_count();
    for (Vertex v = 0; v < graph.size(); ++v)
        for (const auto word : graph.adjacency_words(v))
            rng = (rng ^ word) * 1099511628211ULL;
    while (Clock::now() < deadline) {
        auto seed = grasp_ordering(graph, rng);
        if (stats) ++stats->heuristic_grasp_constructions;
        accept(vns_improve(graph, std::move(seed), deadline, stats));
    }
    // Independent recomputation is the only value allowed to leave the
    // portfolio and alter the verified upper bound.
    incumbent.first = graph.ordering_cutwidth(incumbent.second);
    if (stats) stats->heuristic_runtime_seconds +=
        std::chrono::duration<double>(Clock::now() - portfolio_started).count();
    return incumbent;
}

std::uint32_t degree_lower_bound(const Graph& graph) {
    std::uint32_t maximum = 0;
    for (Vertex v = 0; v < graph.size(); ++v) maximum = std::max(maximum, graph.degree(v));
    return (maximum + 1) / 2;
}

struct RootBounds {
    std::uint32_t degree = 0;
    std::uint32_t density = 0;
    std::uint32_t average_degree = 0;
    std::uint32_t grooming = 0;
    [[nodiscard]] std::uint32_t combined() const {
        return std::max({degree, density, average_degree, grooming});
    }
};

RootBounds root_lower_bounds(const Graph& graph) {
    const std::uint64_t left = graph.size() / 2;
    const std::uint64_t right = graph.size() - left;
    // Every layout contains a prefix of size floor(n/2). At most C(left,2)
    // and C(right,2) edges can stay within its two sides; all remaining edges
    // must cross that bisection.
    const std::uint64_t internal_capacity =
        left * (left - (left != 0)) / 2 + right * (right - (right != 0)) / 2;
    RootBounds result;
    result.degree = degree_lower_bound(graph);
    result.density = graph.edge_count() > internal_capacity
        ? static_cast<std::uint32_t>(graph.edge_count() - internal_capacity)
        : 0U;
    result.average_degree = average_degree_lower_bound(graph.size(), graph.edge_count());
    result.grooming = grooming_density_lower_bound(graph.size(), graph.edge_count());
    return result;
}

std::chrono::milliseconds remaining_time(
    Clock::time_point start, std::chrono::milliseconds limit) {
    if (limit.count() == 0) return std::chrono::milliseconds{0};
    const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    if (used >= limit) return std::chrono::milliseconds{1};
    return limit - used;
}

void accumulate(OptimizerV2Stats& total, const DecisionStats& part) {
    total.parallel_workers_used = std::max(
        total.parallel_workers_used, part.parallel_workers_used);
    total.parallel_root_tasks_started += part.parallel_root_tasks_started;
    total.parallel_root_tasks_completed += part.parallel_root_tasks_completed;
    total.nodes_expanded += part.nodes_expanded;
    total.children_rejected_by_cut += part.children_rejected_by_cut;
    total.failed_cache_hits += part.failed_cache_hits;
    total.failed_states_recorded += part.failed_states_recorded;
    total.twin_symmetric_children_skipped += part.twin_symmetric_children_skipped;
    total.depth_two_lookahead_checks += part.depth_two_lookahead_checks;
    total.children_rejected_by_depth_two_lookahead +=
        part.children_rejected_by_depth_two_lookahead;
    total.cache_strengthenings += part.failed_state_bounds_strengthened;
    total.cache_insertions_skipped += part.failed_state_insertions_skipped;
    total.cache_collisions += part.cache_collisions;
    total.cache_segment_growths += part.cache_segment_growths;
    total.cache_lookup_probes += part.cache_lookup_probes;
    total.cache_insertion_probes += part.cache_insertion_probes;
    total.cache_probes_avoided_after_saturation +=
        part.cache_probes_avoided_after_saturation;
    total.cache_page_promotions += part.cache_page_promotions;
    total.cache_page_second_chances += part.cache_page_second_chances;
    total.cache_pages_recycled += part.cache_pages_recycled;
    total.cache_replacement_admissions += part.cache_replacement_admissions;
    total.cache_entries_evicted += part.cache_entries_evicted;
    total.cache_evicted_depth_sum += part.cache_evicted_depth_sum;
    total.cache_maximum_evicted_depth = std::max(
        total.cache_maximum_evicted_depth, part.cache_maximum_evicted_depth);
    total.unique_canonical_claims += part.unique_canonical_claims;
    total.duplicate_ownership_waits += part.duplicate_ownership_waits;
    total.ownership_saturation += part.ownership_saturation;
    total.cache_peak_entries = std::max(total.cache_peak_entries, part.failed_state_cache_size);
    total.cache_peak_capacity = std::max(total.cache_peak_capacity, part.failed_state_cache_capacity);
    total.cache_peak_memory_bytes = std::max(
        total.cache_peak_memory_bytes, part.failed_state_cache_memory_bytes);
    total.cache_bytes_per_state = total.cache_peak_entries == 0 ? 0.0 :
        static_cast<double>(total.cache_peak_memory_bytes) /
        static_cast<double>(total.cache_peak_entries);
    total.cache_saturated = total.cache_saturated ||
        part.failed_state_insertions_skipped != 0 || part.cache_pages_recycled != 0;
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
    total.residual_histogram_updates += part.residual_histogram_updates;
    total.node_sorts_avoided += part.node_sorts_avoided;
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
    total.partial_bounds.evaluations += part.partial_bounds.evaluations;
    total.partial_bounds.residual_degree_evaluations += part.partial_bounds.residual_degree_evaluations;
    total.partial_bounds.edge_distance_area_evaluations += part.partial_bounds.edge_distance_area_evaluations;
    total.partial_bounds.degree_distance_area_evaluations += part.partial_bounds.degree_distance_area_evaluations;
    total.partial_bounds.degeneracy_evaluations += part.partial_bounds.degeneracy_evaluations;
    total.partial_bounds.residual_degree_prunes += part.partial_bounds.residual_degree_prunes;
    total.partial_bounds.edge_distance_area_prunes += part.partial_bounds.edge_distance_area_prunes;
    total.partial_bounds.degree_distance_area_prunes += part.partial_bounds.degree_distance_area_prunes;
    total.partial_bounds.degeneracy_prunes += part.partial_bounds.degeneracy_prunes;
    total.partial_bounds.residual_degree_session_ceiling_skips +=
        part.partial_bounds.residual_degree_session_ceiling_skips;
    total.partial_bounds.degeneracy_session_ceiling_skips +=
        part.partial_bounds.degeneracy_session_ceiling_skips;
    total.partial_bounds.expensive_slack_gate_skips +=
        part.partial_bounds.expensive_slack_gate_skips;
    total.partial_bounds.lagrangian_evaluations += part.partial_bounds.lagrangian_evaluations;
    total.partial_bounds.lagrangian_mincuts += part.partial_bounds.lagrangian_mincuts;
    total.partial_bounds.lagrangian_certified_prunes += part.partial_bounds.lagrangian_certified_prunes;
    total.partial_bounds.lagrangian_slack_gate_skips += part.partial_bounds.lagrangian_slack_gate_skips;
    total.partial_bounds.lagrangian_residual_gate_skips += part.partial_bounds.lagrangian_residual_gate_skips;
    total.partial_bounds.lagrangian_ineligible_gate_skips += part.partial_bounds.lagrangian_ineligible_gate_skips;
    total.partial_bounds.lagrangian_overflow_gate_skips += part.partial_bounds.lagrangian_overflow_gate_skips;
    total.sdp_state_requests += part.sdp_requests;
    total.sdp_state_certified += part.sdp_certified;
    total.sdp_state_prunes += part.sdp_prunes;
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
    total.residual_dp_peak_bytes = std::max(total.residual_dp_peak_bytes, part.residual_dp_peak_bytes);
    total.residual_dp_seconds += part.residual_dp_seconds;
    total.residual_dp_cold_restarts += part.residual_dp_cold_restarts;
}

OptimizerV2Result optimize_connected(const Graph& graph,
                                     const OptimizerV2Options& options,
                                     Clock::time_point global_start,
                                     const std::pair<std::uint32_t,
                                         std::vector<Vertex>>* initial_seed = nullptr) {
    OptimizerV2Result result;
    std::ofstream trace;
    if (options.strategy_trace) {
        trace.open(*options.strategy_trace, std::ios::out | std::ios::app);
        if (!trace) throw std::runtime_error("cannot open strategy trace");
    }
    std::optional<AdaptiveCheckpoint> resumed_checkpoint;
    if (options.resume) {
        if (options.controller != ControllerMode::adaptive ||
            options.proof_backend != ProofBackend::dfs)
            throw std::invalid_argument("resume requires the adaptive DFS controller");
        resumed_checkpoint = read_adaptive_checkpoint(*options.resume);
        validate_checkpoint_compatibility(
            *resumed_checkpoint, adaptive_checkpoint_compatibility(graph, options));
        if (resumed_checkpoint->vertex_count != graph.size() ||
            !graph.validate_ordering(resumed_checkpoint->ordering))
            throw std::invalid_argument("checkpoint incumbent is not an ordering of this graph");
        const auto verified_width = graph.ordering_cutwidth(resumed_checkpoint->ordering);
        if (verified_width != resumed_checkpoint->upper_bound)
            throw std::invalid_argument("checkpoint incumbent width failed recomputation");
        result.stats.resumed_from_checkpoint = true;
        result.stats.checkpoint_elapsed_milliseconds =
            resumed_checkpoint->elapsed_milliseconds;
    }
    std::pair<std::uint32_t, std::vector<Vertex>> seed;
    if (resumed_checkpoint) {
        seed = {resumed_checkpoint->upper_bound, resumed_checkpoint->ordering};
    } else if (initial_seed) {
        seed = *initial_seed;
    } else if (options.controller == ControllerMode::adaptive) {
        auto ordering = greedy_order(graph, 0, global_start, {});
        if (!ordering) throw std::logic_error("unlimited greedy seed was interrupted");
        seed = {graph.ordering_cutwidth(*ordering), std::move(*ordering)};
    } else {
        seed = heuristic(graph, global_start, options.time_limit, options, &result.stats);
    }
    auto upper = seed.first;
    auto order = std::move(seed.second);
    const bool adaptive_bounds_enabled = options.controller != ControllerMode::adaptive ||
        std::find(options.adaptive_arms.begin(), options.adaptive_arms.end(), "bounds") !=
            options.adaptive_arms.end();
    const auto root_bounds = adaptive_bounds_enabled
        ? root_lower_bounds(graph) : RootBounds{};
    std::uint32_t lower = resumed_checkpoint
        ? std::max(root_bounds.combined(), resumed_checkpoint->lower_bound)
        : root_bounds.combined();
    result.lower_bound = lower;
    result.upper_bound = upper;
    result.ordering = order;
    result.stats.root_degree_bound = root_bounds.degree;
    result.stats.root_density_bound = root_bounds.density;
    result.stats.root_average_degree_bound = root_bounds.average_degree;
    result.stats.root_grooming_bound = root_bounds.grooming;
    auto external_deadline = [&] {
        if (options.time_limit.count() == 0) return Clock::time_point::max();
        return global_start + options.time_limit;
    };
    const bool use_incumbent_arm = options.controller == ControllerMode::adaptive &&
        options.proof_backend == ProofBackend::dfs &&
        std::find(options.adaptive_arms.begin(), options.adaptive_arms.end(), "alns") !=
            options.adaptive_arms.end();
    const bool use_pb_sat_root_arm = options.controller == ControllerMode::adaptive &&
        options.proof_backend == ProofBackend::dfs &&
        std::find(options.adaptive_arms.begin(), options.adaptive_arms.end(), "pb-sat-root") !=
            options.adaptive_arms.end();
    std::unique_ptr<PersistentIncumbentSession> incumbent_session;
    std::uint64_t incumbent_quantum = 1;
    if (use_incumbent_arm) {
        if (resumed_checkpoint && resumed_checkpoint->incumbent)
            incumbent_session = std::make_unique<PersistentIncumbentSession>(
                graph, *resumed_checkpoint->incumbent);
        else
            incumbent_session = std::make_unique<PersistentIncumbentSession>(graph, order);
    }
    auto service_incumbent = [&](std::optional<double> measured_wall_budget = std::nullopt) {
        if (!incumbent_session || lower + 1 >= upper) return;
        auto deadline = external_deadline();
        if (measured_wall_budget && *measured_wall_budget > 0.0) {
            const auto local_deadline = Clock::now() +
                std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(*measured_wall_budget));
            deadline = std::min(deadline, local_deadline);
        }
        const auto service = incumbent_session->service(incumbent_quantum, deadline);
        if (service.improved && service.width < upper) {
            upper = service.width;
            order = service.ordering;
            incumbent_quantum = 1;
        } else if (incumbent_quantum <= std::numeric_limits<std::uint64_t>::max() / 2U) {
            incumbent_quantum *= 2U;
        }
    };
    const auto auxiliary_worker_slots = static_cast<std::size_t>(use_incumbent_arm) +
        static_cast<std::size_t>(use_pb_sat_root_arm);
    const auto dfs_worker_slots = options.threads > auxiliary_worker_slots
        ? options.threads - auxiliary_worker_slots : std::size_t{1};
    if (options.controller == ControllerMode::adaptive) {
        result.stats.adaptive_dfs_worker_allocation = dfs_worker_slots;
        result.stats.adaptive_incumbent_worker_allocation =
            use_incumbent_arm && options.threads > 1 ? 1U : 0U;
    }
    std::unique_ptr<ResidualDpSession> residual_dp_session;
    std::uint64_t residual_dp_quantum = 1;
    bool run_residual_dp_next = false;
    const bool use_residual_dp = options.controller == ControllerMode::adaptive &&
        std::find(options.adaptive_arms.begin(), options.adaptive_arms.end(), "residual-dp") !=
            options.adaptive_arms.end() &&
        !(options.dfs_residual_dp_max_remaining > 0 && graph.size() <= options.dfs_residual_dp_max_remaining);
    const bool use_progressive_cheap_bounds = options.controller == ControllerMode::adaptive &&
        std::find(options.adaptive_arms.begin(), options.adaptive_arms.end(), "bounds") !=
            options.adaptive_arms.end();
    std::unique_ptr<ProgressiveCheapBoundSession> progressive_cheap_bounds;
    std::shared_ptr<ParallelDecisionSession> cheap_bound_forest;
    std::uint64_t cheap_bound_generation = 0;
    bool run_cheap_bounds_next = false;
    if (use_progressive_cheap_bounds)
        progressive_cheap_bounds = std::make_unique<ProgressiveCheapBoundSession>(
            graph, options.partial_bounds);
    if (use_residual_dp) {
        std::vector<Graph::Mask> empty(graph.word_count(), 0);
        const auto projection = project_residual_dp(graph.size(), graph.word_count());
        result.stats.residual_dp_applicable = projection.has_value();
        if (projection)
            result.stats.residual_dp_projected_bytes = projection->peak_bytes;
        if (!projection) {
            result.stats.residual_dp_skip_reason = "projection-unavailable";
        } else if (!options.memory_governor) {
            result.stats.residual_dp_skip_reason = "missing-memory-governor";
        } else if (options.residual_dp_max_bytes != 0 &&
                   projection->peak_bytes > options.residual_dp_max_bytes) {
            result.stats.residual_dp_skip_reason = "projected-bytes-exceed-cap";
        } else {
            if (resumed_checkpoint && resumed_checkpoint->residual_dp) {
                residual_dp_session = std::make_unique<ResidualDpSession>(
                    ResidualDpSession::restore(graph, *resumed_checkpoint->residual_dp, options.memory_governor));
            } else {
                residual_dp_session = std::make_unique<ResidualDpSession>(
                    graph, empty, options.memory_governor);
            }
            result.stats.residual_dp_admitted = residual_dp_session->applicable();
            if (!result.stats.residual_dp_admitted)
                result.stats.residual_dp_skip_reason = "governor-rejected";
        }
        if (trace.is_open()) {
            trace << "{\"monotonic_milliseconds\":"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         Clock::now() - global_start).count()
                  << ",\"event_reason\":\"residual-dp-admission\""
                  << ",\"residual_dp_projected_bytes\":"
                  << result.stats.residual_dp_projected_bytes
                  << ",\"residual_dp_admitted\":"
                  << (result.stats.residual_dp_admitted ? "true" : "false")
                  << ",\"residual_dp_skip_reason\":\""
                  << result.stats.residual_dp_skip_reason << "\"}\n";
            trace.flush();
            if (!trace) throw std::runtime_error("cannot append strategy trace");
        }
    }
    std::size_t next_milestone = 0;
    auto capture_milestones = [&] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - global_start);
        while (next_milestone < options.milestones.size() &&
               (elapsed >= options.milestones[next_milestone] || lower == upper)) {
            const auto elapsed_seconds = std::chrono::duration<double>(elapsed).count();
            const auto incumbent_busy = incumbent_session
                ? incumbent_session->stats().service_seconds : 0.0;
            result.stats.milestones.push_back({
                static_cast<std::uint64_t>(options.milestones[next_milestone].count()),
                static_cast<std::uint64_t>(elapsed.count()),
                lower, upper, result.stats.nodes_expanded,
                result.stats.decision_calls,
                elapsed_seconds * static_cast<double>(options.threads),
                result.stats.busy_worker_seconds + incumbent_busy,
                result.stats.controller_overhead_seconds,
                lower == upper});
            ++next_milestone;
        }
    };

    std::shared_ptr<sdp::SdpBoundOracle> state_oracle;
    const bool adaptive_sdp_enabled = options.controller != ControllerMode::adaptive ||
        std::find(options.adaptive_arms.begin(), options.adaptive_arms.end(), "sdp") !=
            options.adaptive_arms.end();
    if (adaptive_sdp_enabled && options.proof_backend == ProofBackend::dfs &&
        options.sdp_schedule != sdp::SdpSchedule::off && graph.size() > 1) {
        sdp::SdpBoundOracleOptions oracle_options;
        oracle_options.schedule = options.sdp_schedule;
        oracle_options.total_time = options.sdp_total_time;
        oracle_options.max_calls = options.sdp_max_calls;
        oracle_options.max_state_dimension = options.sdp_max_state_dimension;
        oracle_options.trigger_nodes = options.sdp_trigger_nodes;
        oracle_options.max_iterations = options.sdp_iterations == 0 ? 200 : options.sdp_iterations;
        oracle_options.quantization_bits = options.sdp_quantization_bits;
        state_oracle = std::make_shared<sdp::SdpBoundOracle>(graph, oracle_options);
        // Static policies preserve their established eager root query.  The
        // adaptive policy instead admits that exact same root request through
        // a logical, one-task SDP session after a bounded DFS epoch.
        if (options.controller != ControllerMode::adaptive) {
            std::vector<Graph::Mask> empty(graph.word_count(), 0);
            sdp::SdpBoundRequest request;
            request.prefix = empty;
            request.cardinality = graph.size() / 2;
            request.root = true;
            request.existing_certified_bound = lower;
            if (options.time_limit.count() != 0)
                request.caller_deadline = global_start + options.time_limit;
            const auto root_sdp = state_oracle->bound(request);
            if (root_sdp.certified_lower_bound) {
                lower = std::max(lower, *root_sdp.certified_lower_bound);
                result.stats.sdp_certified_lower_bound = root_sdp.certified_lower_bound;
            }
        }
    }
    std::unique_ptr<sdp::SdpBoundOracleAdapter> progressive_sdp_oracle;
    std::unique_ptr<sdp::ProgressiveSdpSession> progressive_sdp_session;
    bool run_sdp_next = false;
    if (options.controller == ControllerMode::adaptive && state_oracle) {
        progressive_sdp_oracle = std::make_unique<sdp::SdpBoundOracleAdapter>(*state_oracle);
        progressive_sdp_session = std::make_unique<sdp::ProgressiveSdpSession>(
            *progressive_sdp_oracle);
    }
    if (resumed_checkpoint) {
        if (progressive_sdp_session && resumed_checkpoint->progressive_sdp)
            progressive_sdp_session->restore(*resumed_checkpoint->progressive_sdp);
        if (progressive_cheap_bounds && resumed_checkpoint->progressive_cheap_bounds)
            progressive_cheap_bounds->restore(*resumed_checkpoint->progressive_cheap_bounds);
    }

    // Strong heuristics are commonly within a few units of optimum. Probe
    // immediately below the incumbent first; after several successful drops,
    // switch to binary search to avoid a long linear walk.
    unsigned descending_steps = 0;
    bool descending = true;
    ThresholdPortfolio threshold_portfolio;
    if (options.node_memo_depth > 4)
        throw std::invalid_argument("node memo depth must be between 0 and 4");
    if (options.node_memo_depth != 0 && options.failed_state_cache_memory_bytes != 0 &&
        options.node_memo_memory_bytes > options.failed_state_cache_memory_bytes)
        throw std::invalid_argument("node memo memory exceeds cache memory");
    auto failed_memory = options.failed_state_cache_memory_bytes;
    std::shared_ptr<NodeMemoTable> node_memo;
    if (options.node_memo_depth != 0 && graph.supports_mask() &&
        options.node_memo_memory_bytes != 0) {
        try { node_memo = std::make_shared<NodeMemoTable>(options.node_memo_memory_bytes); }
        catch (const std::bad_alloc&) { node_memo.reset(); }
    }
    if (node_memo && failed_memory != 0) failed_memory -= options.node_memo_memory_bytes;
    result.stats.node_memo_available = options.node_memo_depth != 0 && graph.supports_mask();
    if (node_memo) result.stats.node_memo_memory_bytes = node_memo->stats().memory_bytes;
    DecisionCacheOptions cache_config{options.failed_state_cache_limit, failed_memory};
    cache_config.replacement = options.cache_replacement;
    cache_config.replacement_page_capacity = options.cache_replacement_page_capacity;
    std::unique_ptr<Word64DecisionCache> small_cache;
    std::unique_ptr<DynamicDecisionCache> dynamic_cache;
    std::unique_ptr<ShardedDynamicDecisionCache> parallel_dynamic_cache;
    std::unique_ptr<pb::IncrementalCadicalSession> incremental_cadical;
    std::optional<pb::CutwidthCnf> incremental_encoding;
    std::optional<std::uint32_t> incremental_threshold;
    struct AdaptiveSessionEntry {
        std::unique_ptr<DecisionSession> serial;
        std::shared_ptr<ParallelDecisionSession> parallel;
        std::shared_ptr<ParallelGlobalDFSSession> global_parallel;
        std::uint64_t quantum = 1;
        std::uint64_t services = 0;
        std::uint64_t generation = 0;

        // Telemetry stats for value-aware scheduling
        std::uint64_t cumulative_nodes = 0;
        double cumulative_busy_seconds = 0.0;
        double cumulative_allocated_seconds = 0.0;
        std::uint64_t starvation_ticks = 0;
        std::uint64_t cumulative_resolved_regions = 0;
        std::uint64_t cumulative_configured_regions = 0;

        void cancel() {
            if (serial) serial->cancel();
            if (parallel) parallel->cancel();
        }
        SessionStatus status() const {
            return serial ? serial->status() : parallel->status();
        }
        std::vector<Graph::Vertex> ordering() const {
            return serial ? serial->ordering() : parallel->ordering();
        }
    };
    std::unordered_map<std::uint32_t, AdaptiveSessionEntry> adaptive_sessions;
    // The executor is deliberately controller-owned.  It exists only for the
    // adaptive parallel forest path: legacy serial and static policies retain
    // their established service ownership.
    const bool use_global_parallel_dfs = options.controller == ControllerMode::adaptive &&
        options.proof_backend == ProofBackend::dfs &&
        // A single coarse worker already owns exactly one proof forest; the
        // shared-pool protocol adds no scheduling capability there and its
        // lease boundary can repeatedly censor a recursive continuation.
        // Keep that established one-worker path owned by the session.
        dfs_worker_slots > 1;
    std::unique_ptr<GlobalDFSExecutor> global_dfs_executor;
    if (use_global_parallel_dfs) {
        global_dfs_executor = std::make_unique<GlobalDFSExecutor>(
            dfs_worker_slots, std::chrono::milliseconds(10));
        global_dfs_executor->set_overall_deadline(external_deadline());
    }
    std::uint64_t next_session_generation = 1;
    std::unordered_map<std::uint32_t, SessionSnapshot> resumed_serial_sessions;
    std::unordered_map<std::uint32_t, ParallelDecisionSnapshot> resumed_parallel_sessions;
    if (resumed_checkpoint) {
        for (auto& snapshot : resumed_checkpoint->sessions)
            resumed_serial_sessions.emplace(snapshot.threshold, std::move(snapshot));
        for (auto& snapshot : resumed_checkpoint->parallel_sessions) {
            snapshot.fixed_cache = std::nullopt;
            resumed_parallel_sessions.emplace(snapshot.threshold, std::move(snapshot));
        }
    }
    std::vector<CompletedThreshold> completed_thresholds = resumed_checkpoint
        ? resumed_checkpoint->completed_thresholds : std::vector<CompletedThreshold>{};
    ValueAwareThresholdEpoch value_aware_epoch;
    if (options.threshold_scheduler == ThresholdSchedulerMode::value_aware &&
        resumed_checkpoint && resumed_checkpoint->value_aware_epoch) {
        const auto& saved = *resumed_checkpoint->value_aware_epoch;
        value_aware_epoch.restore(
            saved.lower_bound, saved.upper_bound, saved.candidates);
    }
    std::uint32_t scheduled_lower = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t scheduled_upper = std::numeric_limits<std::uint32_t>::max();
    std::size_t adaptive_tick = 0;
    auto write_live_checkpoint = [&] {
        if (!options.checkpoint_out || options.checkpoint_out->empty()) return;
        if (options.controller != ControllerMode::adaptive ||
            options.proof_backend != ProofBackend::dfs) return;
        if (options.time_limit.count() <= 0) return;
        if (lower >= upper) return;
        if (Clock::now() < global_start + options.time_limit) return;
        const auto started = Clock::now();
        // No external lease may overlap proof-forest serialization.  Normal
        // service drains every epoch; this also makes an emergency snapshot
        // safe if a future controller arm exits early.
        if (global_dfs_executor) global_dfs_executor->quiesce_all();
        AdaptiveCheckpoint checkpoint;
        const auto compatibility = adaptive_checkpoint_compatibility(graph, options);
        checkpoint.graph_hash = compatibility.graph_hash;
        checkpoint.solver_semantic_hash = compatibility.solver_semantic_hash;
        checkpoint.proof_policy_hash = compatibility.proof_policy_hash;
        checkpoint.candidate_order_hash = compatibility.candidate_order_hash;
        checkpoint.vertex_count = static_cast<std::uint32_t>(graph.size());
        checkpoint.declared_memory_bytes = options.memory_budget_bytes;
        checkpoint.ordering = order;
        checkpoint.lower_bound = lower;
        checkpoint.upper_bound = upper;
        checkpoint.elapsed_milliseconds =
            (resumed_checkpoint ? resumed_checkpoint->elapsed_milliseconds : 0U) +
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - global_start).count());
        std::sort(completed_thresholds.begin(), completed_thresholds.end(),
            [](const auto& a, const auto& b) { return a.threshold < b.threshold; });
        completed_thresholds.erase(std::unique(completed_thresholds.begin(),
            completed_thresholds.end(), [](const auto& a, const auto& b) {
                return a.threshold == b.threshold && a.result == b.result;
            }), completed_thresholds.end());
        checkpoint.completed_thresholds = completed_thresholds;
        if (options.threshold_scheduler == ThresholdSchedulerMode::value_aware &&
            value_aware_epoch.initialized()) {
            checkpoint.value_aware_epoch = ValueAwareEpochCheckpoint{
                value_aware_epoch.lower(), value_aware_epoch.upper(),
                value_aware_epoch.candidates()};
        }
        if (incumbent_session) checkpoint.incumbent = incumbent_session->snapshot();
        if (progressive_sdp_session)
            checkpoint.progressive_sdp = progressive_sdp_session->snapshot();
        if (progressive_cheap_bounds)
            checkpoint.progressive_cheap_bounds = progressive_cheap_bounds->snapshot();
        if (residual_dp_session && residual_dp_session->applicable())
            checkpoint.residual_dp = residual_dp_session->snapshot();
        for (const auto& [candidate, snapshot] : resumed_serial_sessions)
            if (candidate >= lower && candidate < upper)
                checkpoint.sessions.push_back(snapshot);
        for (const auto& [candidate, snapshot] : resumed_parallel_sessions)
            if (candidate >= lower && candidate < upper)
                checkpoint.parallel_sessions.push_back(snapshot);
        for (const auto& [candidate, entry] : adaptive_sessions) {
            if (candidate < lower || candidate >= upper ||
                entry.status() != SessionStatus::unresolved) continue;

            SessionTelemetry tele;
            tele.nodes = entry.cumulative_nodes;
            tele.busy_seconds = entry.cumulative_busy_seconds;
            tele.allocated_seconds = entry.cumulative_allocated_seconds;
            tele.has_telemetry = true;
            checkpoint.session_telemetry[candidate] = tele;

            if (entry.serial) {
                auto snapshot = entry.serial->quiesce_and_snapshot();
                snapshot.controller_quantum = entry.quantum;
                snapshot.controller_services = entry.services;
                snapshot.session_generation = entry.generation;
                checkpoint.sessions.push_back(std::move(snapshot));
            } else {
                auto snapshot = entry.parallel->quiesce_and_snapshot(SnapshotPolicy::omit_cache);
                snapshot.controller_quantum = entry.quantum;
                snapshot.controller_services = entry.services;
                snapshot.session_generation = entry.generation;
                checkpoint.parallel_sessions.push_back(std::move(snapshot));
            }
        }
        std::sort(checkpoint.sessions.begin(), checkpoint.sessions.end(),
            [](const auto& a, const auto& b) { return a.threshold < b.threshold; });
        std::sort(checkpoint.parallel_sessions.begin(), checkpoint.parallel_sessions.end(),
            [](const auto& a, const auto& b) { return a.threshold < b.threshold; });
        write_adaptive_checkpoint_atomic(*options.checkpoint_out, checkpoint);
        if (global_dfs_executor) global_dfs_executor->resume();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started);
        result.stats.checkpoint_write_seconds +=
            std::chrono::duration<double>(elapsed).count();
        ++result.stats.checkpoints_written;
    };
    // Milestones are observational telemetry. Persisting multi-gigabyte cache
    // snapshots at each milestone consumed the remaining wall budget and
    // changed which thresholds received service. The live forest is written
    // once at the terminal boundary below; explicit resume remains exact.
    if (options.proof_backend == ProofBackend::dfs &&
        options.controller != ControllerMode::adaptive &&
        options.reuse_failed_state_cache_across_thresholds) {
        const bool word64 = options.backend == DecisionBackend::word64 ||
            (options.backend == DecisionBackend::automatic && graph.supports_mask());
        if (!word64 && options.threads > 1) {
            const auto shards = std::max(
                options.parallel_min_cache_shards,
                options.threads * options.parallel_cache_shards_per_thread);
            parallel_dynamic_cache = std::make_unique<ShardedDynamicDecisionCache>(
                graph.word_count(), shards, cache_config);
        } else if (word64 && options.threads == 1) {
            if (!graph.supports_mask())
                throw std::invalid_argument("word64 backend does not support more than 63 vertices");
            small_cache = std::make_unique<Word64DecisionCache>(cache_config);
        } else if (!word64) {
            dynamic_cache = std::make_unique<DynamicDecisionCache>(
                graph.word_count(), cache_config);
        }
    }
    std::shared_ptr<PbSatRootJobState> active_pb_sat_root_job;
    std::unique_ptr<std::jthread> pb_sat_root_thread;
    std::set<std::uint32_t> pb_sat_root_attempted_thresholds;

    while (lower < upper) {
        capture_milestones();
        // 1. Poll the pb-sat-root job status if one is running
        if (active_pb_sat_root_job) {
            const auto current_status = active_pb_sat_root_job->status.load(
                std::memory_order_acquire);
            if (current_status == PbSatRootJobState::Status::certified_unsat ||
                current_status == PbSatRootJobState::Status::sat ||
                current_status == PbSatRootJobState::Status::timed_out ||
                current_status == PbSatRootJobState::Status::failed) {

                // Join after observing the release-published terminal state so
                // all non-atomic result fields are safe to consume.
                pb_sat_root_thread.reset();
                result.stats.pb_sat_root_solver_seconds += active_pb_sat_root_job->solver_seconds;
                result.stats.pb_sat_root_checker_seconds += active_pb_sat_root_job->checker_seconds;
                result.stats.pb_sat_root_active_threshold = active_pb_sat_root_job->threshold;
                result.stats.pb_sat_root_active_cardinality = active_pb_sat_root_job->cardinality;
                result.stats.pb_sat_root_last_cnf_path = active_pb_sat_root_job->cnf_path;
                result.stats.pb_sat_root_last_proof_path = active_pb_sat_root_job->proof_path;

                if (current_status == PbSatRootJobState::Status::certified_unsat) {
                    result.stats.pb_sat_root_certified_unsat++;
                    result.stats.pb_sat_root_checker_successes++;
                    result.stats.pb_sat_root_last_result = "CERTIFIED_UNSAT";

                    lower = std::max(lower, active_pb_sat_root_job->threshold + 1U);
                } else if (current_status == PbSatRootJobState::Status::sat) {
                    result.stats.pb_sat_root_sat++;
                    result.stats.pb_sat_root_last_result = "SAT";
                } else if (current_status == PbSatRootJobState::Status::timed_out) {
                    result.stats.pb_sat_root_timeouts++;
                    result.stats.pb_sat_root_last_result = "TIMEOUT";
                } else {
                    result.stats.pb_sat_root_failures++;
                    result.stats.pb_sat_root_last_result = "FAILURE";
                }

                active_pb_sat_root_job.reset();
            }
        }
        if (lower >= upper) {
            for (auto& [unused_threshold, entry] : adaptive_sessions) {
                (void)unused_threshold;
                entry.cancel();
            }
            break;
        }

        // 2. Spawn the root proof once the configured certified-gap gate opens.
        if (use_pb_sat_root_arm && !active_pb_sat_root_job &&
            (upper - lower <= options.pb_sat_root_max_gap)) {
            std::uint32_t K = upper - 1;
            if (pb_sat_root_attempted_thresholds.count(K) == 0) {
                pb_sat_root_attempted_thresholds.insert(K);
                std::size_t q = options.pb_sat_root_q.value_or(graph.size() / 2);

                active_pb_sat_root_job = std::make_shared<PbSatRootJobState>();
                ++result.stats.pb_sat_root_attempts;
                result.stats.pb_sat_root_active_threshold = K;
                result.stats.pb_sat_root_active_cardinality = q;
                pb_sat_root_thread = std::make_unique<std::jthread>(
                    run_pb_sat_root_job,
                    std::ref(graph),
                    options.pb_sat_root_solver,
                    options.pb_sat_root_checker,
                    options.pb_sat_root_dir,
                    options.pb_sat_root_timeout,
                    q,
                    K,
                    active_pb_sat_root_job
                );
            }
        }

        if (Clock::now() >= external_deadline()) break;
        if (global_dfs_executor) {
            global_dfs_executor->set_overall_deadline(external_deadline());
        }
        const auto interval_lower_before = lower;
        const auto interval_upper_before = upper;
        std::uint32_t threshold = descending ? upper - 1 : lower + (upper - lower) / 2;
        bool traced_adaptive_service = false;
        bool traced_created = false;
        std::uint64_t traced_quantum = 0;
        std::uint64_t traced_generation = 0;
        std::size_t traced_workers_used = 0;
        double traced_busy_worker_seconds = 0.0;
        double traced_allocated_worker_seconds = 0.0;
        double traced_service_seconds = 0.0;
        double traced_incumbent_busy_seconds = 0.0;
        std::uint64_t traced_incumbent_service_calls = 0;
        std::uint64_t traced_incumbent_candidate_evaluations = 0;
        bool traced_incumbent_concurrent = false;
        std::shared_ptr<ParallelDecisionSession> traced_parallel_forest;
        SessionYieldReason traced_reason = SessionYieldReason::quantum_complete;
        std::vector<std::uint32_t> active_secondaries;
        if (options.controller == ControllerMode::adaptive &&
            options.proof_backend == ProofBackend::dfs) {
            if (lower != scheduled_lower || upper != scheduled_upper) {
                scheduled_lower = lower;
                scheduled_upper = upper;
                adaptive_tick = 0;
                for (auto it = adaptive_sessions.begin(); it != adaptive_sessions.end();) {
                    if (it->first < lower || it->first >= upper) {
                        it->second.cancel();
                        if (progressive_sdp_session)
                            progressive_sdp_session->deactivate_threshold(it->first);
                        if (progressive_cheap_bounds)
                            progressive_cheap_bounds->deactivate_threshold(it->first);
                        if (global_dfs_executor && it->second.global_parallel)
                            global_dfs_executor->unregister_session(
                                it->second.global_parallel);
                        it = adaptive_sessions.erase(it);
                    } else ++it;
                }
            }
            if (use_residual_dp && residual_dp_session && residual_dp_session->applicable() &&
                !residual_dp_session->complete() && run_residual_dp_next &&
                !global_dfs_executor) {
                const auto deadline = external_deadline();
                const auto service_started = Clock::now();
                constexpr std::uint64_t MAX_RESIDUAL_DP_QUANTUM = 1024;
                const auto current_quantum = std::min(residual_dp_quantum, MAX_RESIDUAL_DP_QUANTUM);
                const auto event = residual_dp_session->service(current_quantum, deadline);

                ++result.stats.residual_dp_service_calls;
                result.stats.residual_dp_states += event.states_completed;
                if (event.complete && event.exact_completion) {
                    lower = std::max(lower, *event.exact_completion);
                    result.stats.residual_dp_completed = true;
                }
                if (!event.complete && residual_dp_quantum <= std::numeric_limits<std::uint64_t>::max() / 2U) {
                    residual_dp_quantum *= 2U;
                }

                if (trace.is_open()) {
                    trace << "{\"monotonic_milliseconds\":"
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - global_start).count()
                          << ",\"threshold\":" << threshold
                          << ",\"session_generation\":0"
                          << ",\"interval_before\":[" << interval_lower_before << ','
                          << interval_upper_before << "]"
                          << ",\"interval_after\":[" << lower << ',' << upper << "]"
                          << ",\"service_quantum\":" << current_quantum
                          << ",\"worker_allocation\":0"
                          << ",\"workers_used\":0"
                          << ",\"incumbent_worker_allocation\":0"
                          << ",\"incumbent_service_calls\":0"
                          << ",\"incumbent_candidate_evaluations\":0"
                          << ",\"incumbent_busy_seconds\":0.0"
                          << ",\"busy_worker_seconds\":" << std::chrono::duration<double>(Clock::now() - service_started).count()
                          << ",\"allocated_worker_seconds\":" << std::chrono::duration<double>(Clock::now() - service_started).count()
                          << ",\"event_reason\":\"residual-dp\""
                          << ",\"switch_reason\":\"residual-dp\""
                          << ",\"right_censored\":" << (event.complete ? "false" : "true")
                          << ",\"nodes_expanded\":0"
                          << ",\"residual_dp_states\":" << event.states_completed
                          << ",\"residual_dp_attempts\":" << result.stats.residual_dp_attempts
                          << ",\"residual_dp_admissions\":" << result.stats.residual_dp_admissions
                          << ",\"residual_dp_rejections\":" << result.stats.residual_dp_governor_or_cap_rejections
                          << ",\"residual_dp_completed_tails\":" << result.stats.residual_dp_completed_tails
                          << ",\"residual_dp_infeasible_prunes\":" << result.stats.residual_dp_infeasible_prunes
                          << ",\"residual_dp_feasible_witnesses\":" << result.stats.residual_dp_feasible_witnesses
                          << ",\"residual_dp_peak_bytes\":" << result.stats.residual_dp_peak_bytes
                          << ",\"residual_dp_seconds\":" << result.stats.residual_dp_seconds
                          << ",\"residual_dp_cold_restarts\":" << result.stats.residual_dp_cold_restarts
                          << "}\n";
                    trace.flush();
                    if (!trace) throw std::runtime_error("cannot append strategy trace");
                }

                run_residual_dp_next = false;
                capture_milestones();
                continue;
            }
            // The legacy serial path keeps its established ownership.  A
            // global DFS window instead admits SDP through one shared-pool
            // lease below: do not run the same queued task serially before
            // the pool gets a chance to service it.
            if (progressive_sdp_session && run_sdp_next && !global_dfs_executor) {
                const auto service_started = Clock::now();
                const auto event = progressive_sdp_session->service_one(external_deadline());
                if (event.committed) {
                    const auto& certified = progressive_sdp_session->certified_lower_bound();
                    if (certified) {
                        lower = std::max(lower, *certified);
                        result.stats.sdp_certified_lower_bound = certified;
                    }
                }
                if (trace.is_open()) {
                    const auto event_threshold = event.task ? event.task->threshold : threshold;
                    const auto event_generation = event.task ? event.task->generation : 0U;
                    trace << "{\"monotonic_milliseconds\":"
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - global_start).count()
                          << ",\"arm\":\"sdp\""
                          << ",\"threshold\":" << event_threshold
                          << ",\"session_generation\":" << event_generation
                          << ",\"interval_before\":[" << interval_lower_before << ','
                          << interval_upper_before << "]"
                          << ",\"interval_after\":[" << lower << ',' << upper << "]"
                          << ",\"service_quantum\":1"
                          << ",\"worker_allocation\":0,\"workers_used\":0"
                          << ",\"busy_worker_seconds\":"
                          << std::chrono::duration<double>(Clock::now() - service_started).count()
                          << ",\"allocated_worker_seconds\":0.0"
                          << ",\"event_reason\":\"sdp\""
                          << ",\"switch_reason\":\"sdp\""
                          << ",\"right_censored\":"
                          << (event.committed ? "false" : "true")
                          << ",\"certified_contribution\":\""
                          << (event.committed ? "certified_lower_bound" : "none") << "\"}\n";
                    trace.flush();
                    if (!trace) throw std::runtime_error("cannot append strategy trace");
                }
                run_sdp_next = false;
                capture_milestones();
                continue;
            }
            // With the global executor, a claimed proof-forest fragment is
            // evaluated through one pool lease alongside the next DFS
            // window below. Keep the established serial path otherwise.
            if (progressive_cheap_bounds && run_cheap_bounds_next &&
                !(global_dfs_executor && cheap_bound_forest && dfs_worker_slots > 2U)) {
                const auto service_started = Clock::now();
                const auto event = cheap_bound_forest
                    ? progressive_cheap_bounds->service_one(*cheap_bound_forest)
                    : ProgressiveCheapBoundEvent{};
                if (event.task) {
                    result.stats.partial_bounds.evaluations += event.stats.evaluations;
                    result.stats.partial_bounds.residual_degree_evaluations += event.stats.residual_degree_evaluations;
                    result.stats.partial_bounds.edge_distance_area_evaluations += event.stats.edge_distance_area_evaluations;
                    result.stats.partial_bounds.degree_distance_area_evaluations += event.stats.degree_distance_area_evaluations;
                    result.stats.partial_bounds.degeneracy_evaluations += event.stats.degeneracy_evaluations;
                    result.stats.partial_bounds.residual_degree_prunes += event.stats.residual_degree_prunes;
                    result.stats.partial_bounds.edge_distance_area_prunes += event.stats.edge_distance_area_prunes;
                    result.stats.partial_bounds.degree_distance_area_prunes += event.stats.degree_distance_area_prunes;
                    result.stats.partial_bounds.degeneracy_prunes += event.stats.degeneracy_prunes;
                    result.stats.partial_bounds.residual_degree_session_ceiling_skips += event.stats.residual_degree_session_ceiling_skips;
                    result.stats.partial_bounds.degeneracy_session_ceiling_skips += event.stats.degeneracy_session_ceiling_skips;
                    result.stats.partial_bounds.expensive_slack_gate_skips += event.stats.expensive_slack_gate_skips;

                    result.stats.partial_bounds.lagrangian_evaluations += event.stats.lagrangian_evaluations;
                    result.stats.partial_bounds.lagrangian_mincuts += event.stats.lagrangian_mincuts;
                    result.stats.partial_bounds.lagrangian_certified_prunes += event.stats.lagrangian_certified_prunes;
                    result.stats.partial_bounds.lagrangian_slack_gate_skips += event.stats.lagrangian_slack_gate_skips;
                    result.stats.partial_bounds.lagrangian_residual_gate_skips += event.stats.lagrangian_residual_gate_skips;
                    result.stats.partial_bounds.lagrangian_ineligible_gate_skips += event.stats.lagrangian_ineligible_gate_skips;
                    result.stats.partial_bounds.lagrangian_overflow_gate_skips += event.stats.lagrangian_overflow_gate_skips;
                }
                if (trace.is_open()) {
                    const auto event_threshold = event.task ? event.task->threshold : threshold;
                    const auto event_generation = event.task ? event.task->generation : 0U;
                    trace << "{\"monotonic_milliseconds\":"
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - global_start).count()
                          << ",\"arm\":\"bounds\""
                          << ",\"threshold\":" << event_threshold
                          << ",\"session_generation\":" << event_generation
                          << ",\"interval_before\":[" << interval_lower_before << ','
                          << interval_upper_before << "]"
                          << ",\"interval_after\":[" << lower << ',' << upper << "]"
                          << ",\"service_quantum\":1"
                          << ",\"worker_allocation\":0,\"workers_used\":0"
                          << ",\"busy_worker_seconds\":"
                          << std::chrono::duration<double>(Clock::now() - service_started).count()
                          << ",\"allocated_worker_seconds\":0.0"
                          << ",\"event_reason\":\"bounds\""
                          << ",\"switch_reason\":\"bounds\""
                          << ",\"right_censored\":"
                          << (event.certified_prune ? "false" : "true")
                          << ",\"certified_contribution\":\""
                          << (event.certified_prune ? "fragment_infeasible" : "none")
                          << "\"";
                    if (event.lagrangian_evaluated) {
                        trace << ",\"lagrangian_best_bound\":" << event.lagrangian_best_bound
                              << ",\"lagrangian_best_cardinality\":" << event.lagrangian_best_cardinality
                              << ",\"lagrangian_best_numerator\":" << event.lagrangian_best_numerator
                              << ",\"lagrangian_best_denominator\":" << event.lagrangian_best_denominator;
                    }
                    trace << "}\n";
                    trace.flush();
                    if (!trace) throw std::runtime_error("cannot append strategy trace");
                }
                run_cheap_bounds_next = false;
                cheap_bound_forest.reset();
                capture_milestones();
                continue;
            }
            std::vector<std::uint32_t> retained;
            auto retain_live = [&](std::uint32_t candidate) {
                if (candidate >= lower && candidate < upper)
                    retained.push_back(candidate);
            };
            for (const auto& [candidate, unused] : adaptive_sessions) {
                (void)unused;
                retain_live(candidate);
            }
            for (const auto& [candidate, unused] : resumed_serial_sessions) {
                (void)unused;
                retain_live(candidate);
            }
            for (const auto& [candidate, unused] : resumed_parallel_sessions) {
                (void)unused;
                retain_live(candidate);
            }
            // Level-synchronous universal service: every live threshold gets
            // one recurrence at level L before any threshold advances to L+1.
            // This is essential when recurrence quanta double geometrically;
            // frequency bias plus per-arm doubling would otherwise make the
            // primary exponentially dominate wall time. Ties use the stable
            // priority above, so the primary remains the preferred exact arm.
            if (options.threshold_scheduler == ThresholdSchedulerMode::value_aware) {
                auto epoch_retained = retained;
                std::sort(epoch_retained.begin(), epoch_retained.end());
                epoch_retained.erase(
                    std::unique(epoch_retained.begin(), epoch_retained.end()),
                    epoch_retained.end());
                const auto epoch_update = value_aware_epoch.update(
                    lower, upper, epoch_retained);
                const auto& candidates = epoch_update.active_candidates;
                if (candidates.empty())
                    throw std::logic_error("value-aware epoch has no active candidate");
                std::vector<double> scores(candidates.size());
                std::vector<double> progress(candidates.size());
                std::vector<double> probabilities(candidates.size());

                constexpr std::uint64_t MAX_STARVATION_TICKS = 3;

                struct CandidateInfo {
                    std::size_t index;
                    std::uint32_t K;
                    bool is_pilot;
                    std::uint64_t starve_ticks;
                    double score;
                };

                std::vector<CandidateInfo> info(candidates.size());
                bool any_pilot = false;
                std::uint64_t max_starve_ticks = 0;

                for (std::size_t i = 0; i < candidates.size(); ++i) {
                    const auto K = candidates[i];
                    std::uint64_t nodes = 0;
                    double B_K = 0.0;
                    double A_K = 0.0;
                    std::uint64_t starve_ticks = 0;
                    std::uint64_t S_K = 0;
                    std::uint64_t R_res = 0;
                    std::uint64_t R_conf = 0;

                    auto found = adaptive_sessions.find(K);
                    if (found != adaptive_sessions.end()) {
                        nodes = found->second.cumulative_nodes;
                        B_K = found->second.cumulative_busy_seconds;
                        A_K = found->second.cumulative_allocated_seconds;
                        starve_ticks = found->second.starvation_ticks;
                        S_K = found->second.services;
                        R_res = found->second.cumulative_resolved_regions;
                        R_conf = found->second.cumulative_configured_regions;
                    }
                    (void)nodes; // Keep nodes only as telemetry if used elsewhere; do not use it in scoring.

                    const double P_F = get_prior_feasible_probability(lower, upper, K);
                    probabilities[i] = P_F;

                    double expected_reduction = P_F * (upper - K) + (1.0 - P_F) * (K + 1.0 - lower);

                    double P_K = (R_conf > 0) ? std::min(1.0, static_cast<double>(R_res) / static_cast<double>(R_conf)) : 0.0;
                    progress[i] = P_K;

                    double C_prior = get_prior_cost(lower, K);
                    double H = 0.1;
                    double C_K = (B_K * (1.0 - P_K) + C_prior * H) / (P_K + H);
                    double U_K = std::max(0.05, A_K > 0.0 ? (B_K / A_K) : 0.0);
                    double C_adj = C_K / U_K;

                    const bool is_pilot = (B_K == 0.0 || S_K < 1);
                    if (is_pilot) {
                        any_pilot = true;
                    }
                    if (starve_ticks > max_starve_ticks) {
                        max_starve_ticks = starve_ticks;
                    }

                    double score = 0.0;
                    if (is_pilot) {
                        score = 1e15 - K;
                    } else {
                        score = expected_reduction / C_adj;
                    }
                    scores[i] = score;

                    info[i] = CandidateInfo{i, K, is_pilot, starve_ticks, score};
                }

                std::size_t selected_index = 0;
                if (any_pilot) {
                    double max_pilot_score = -std::numeric_limits<double>::infinity();
                    for (std::size_t i = 0; i < info.size(); ++i) {
                        if (info[i].is_pilot) {
                            if (info[i].score > max_pilot_score) {
                                max_pilot_score = info[i].score;
                                selected_index = i;
                            }
                        }
                    }
                } else if (max_starve_ticks >= MAX_STARVATION_TICKS) {
                    for (std::size_t i = 0; i < info.size(); ++i) {
                        if (info[i].starve_ticks == max_starve_ticks) {
                            selected_index = i;
                            break;
                        }
                    }
                } else {
                    double max_ordinary_score = -std::numeric_limits<double>::infinity();
                    for (std::size_t i = 0; i < info.size(); ++i) {
                        if (info[i].score > max_ordinary_score) {
                            max_ordinary_score = info[i].score;
                            selected_index = i;
                        } else if (info[i].score == max_ordinary_score) {
                            if (info[i].K > info[selected_index].K) {
                                selected_index = i;
                            }
                        }
                    }
                }

                threshold = candidates[selected_index];
                ++adaptive_tick;

                if (trace.is_open()) {
                    trace << "{\"monotonic_milliseconds\":"
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - global_start).count()
                          << ",\"arm\":\"scheduler_decision\""
                          << ",\"selected_threshold\":" << threshold
                          << ",\"epoch_bounds\":[" << value_aware_epoch.lower()
                          << ',' << value_aware_epoch.upper() << ']'
                          << ",\"epoch_rebased\":"
                          << (epoch_update.rebased() ? "true" : "false")
                          << ",\"epoch_rebase_reason\":\""
                          << value_aware_rebase_reason_name(epoch_update.reason) << "\""
                          << ",\"candidates\":[";
                    for (std::size_t i = 0; i < candidates.size(); ++i) {
                        if (i > 0) trace << ",";
                        trace << candidates[i];
                    }
                    trace << "],\"active_candidates\":[";
                    for (std::size_t i = 0; i < candidates.size(); ++i) {
                        if (i > 0) trace << ",";
                        trace << candidates[i];
                    }
                    trace << "],\"candidate_scores\":[";
                    for (std::size_t i = 0; i < candidates.size(); ++i) {
                        if (i > 0) trace << ",";
                        trace << scores[i];
                    }
                    trace << "],\"candidate_progress\":[";
                    for (std::size_t i = 0; i < candidates.size(); ++i) {
                        if (i > 0) trace << ",";
                        trace << progress[i];
                    }
                    trace << "],\"candidate_probabilities\":[";
                    for (std::size_t i = 0; i < candidates.size(); ++i) {
                        if (i > 0) trace << ",";
                        trace << probabilities[i];
                    }
                    trace << "]}\n";
                    trace.flush();
                }
            } else {
                const auto candidates = persistent_threshold_candidates(lower, upper, retained);
                std::vector<std::uint64_t> recurrence_levels;
                recurrence_levels.reserve(candidates.size());
                for (std::size_t index = 0; index < candidates.size(); ++index) {
                    const auto found = adaptive_sessions.find(candidates[index]);
                    recurrence_levels.push_back(found == adaptive_sessions.end()
                        ? std::uint64_t{0} : found->second.services);
                }
                const auto selected = select_recurring_threshold(recurrence_levels);
                threshold = candidates[selected];
                ++adaptive_tick;
            }
        } else if (options.controller == ControllerMode::adaptive) {
            const auto adaptive_choice = threshold_portfolio.next(lower, upper);
            if (adaptive_choice) threshold = adaptive_choice->threshold;
        }
        DecisionOptions decision_options;
        decision_options.time_limit = remaining_time(global_start, options.time_limit);
        decision_options.use_failed_state_cache = options.use_failed_state_cache;
        decision_options.use_twin_symmetry = options.use_twin_symmetry;
        decision_options.use_depth_two_lookahead = options.use_depth_two_lookahead;
        decision_options.best_next_buckets = options.best_next_buckets;
        decision_options.candidate_enumerator = options.candidate_enumerator;
        decision_options.local_continuation_depth = options.local_continuation_depth;
        decision_options.local_continuation_max_slack =
            options.local_continuation_max_slack;
        decision_options.local_continuation_max_children =
            options.local_continuation_max_children;
        decision_options.local_continuation_max_states =
            options.local_continuation_max_states;
        decision_options.depth_two_lookahead_max_remaining =
            options.depth_two_lookahead_max_remaining;
        decision_options.failed_state_cache_limit = options.failed_state_cache_limit;
        decision_options.failed_state_cache_memory_bytes = failed_memory;
        decision_options.cache_replacement = options.cache_replacement;
        decision_options.cache_replacement_page_capacity =
            options.cache_replacement_page_capacity;
        // `--cache-memory` is the ceiling for one threshold cache.  The
        // adaptive controller used to right-shift that ceiling by the
        // threshold's portfolio rank so the sum of all caches fit beneath one
        // ceiling.  That freezes a decisive lower-frontier proof with a tiny
        // cache merely because it was discovered after UB-1: on Grid15, K=13
        // received 256 MiB and failed to close after more than twice the work
        // needed with the requested 2 GiB cache.  The memory governor already
        // enforces the independent aggregate `--memory-budget`; let every live
        // threshold request the configured per-cache ceiling and fall back to
        // cacheless search only if that global admission fails.
        decision_options.node_state = options.node_state;
        decision_options.node_order = options.node_order;
        decision_options.node_memo_depth = options.node_memo_depth;
        decision_options.node_memo_max_remaining = options.node_memo_max_remaining;
        decision_options.node_memo_memory_bytes = options.node_memo_memory_bytes;
        decision_options.node_memo = node_memo;
        // Cheap node-local pruning remains part of every DFS expansion. The
        // progressive bounds arm owns whole unstarted proof fragments and is
        // therefore complementary; disabling inline bounds when that arm was
        // enabled multiplied K=12/K=13 proof work without creating useful
        // concurrency.
        decision_options.use_partial_bounds = options.use_partial_bounds;
        decision_options.partial_bounds = options.partial_bounds;
        decision_options.backend = options.backend;
        decision_options.threads = dfs_worker_slots;
        decision_options.parallel_min_cache_shards = options.parallel_min_cache_shards;
        decision_options.parallel_cache_shards_per_thread = options.parallel_cache_shards_per_thread;
        decision_options.parallel_runtime = options.parallel_runtime;
        decision_options.use_canonical_ownership = options.use_canonical_ownership;
        decision_options.cooperative_work_stealing = options.cooperative_work_stealing;
        decision_options.canonical_frontier_bootstrap = options.canonical_frontier_bootstrap;
        decision_options.recursive_coarse_kernel = options.recursive_coarse_kernel;
        decision_options.controller_overhead_fraction =
            options.controller_overhead_fraction;
        decision_options.sdp_oracle = state_oracle;
        decision_options.max_proof_regions = options.max_proof_regions;
        decision_options.dfs_residual_dp_max_remaining = options.dfs_residual_dp_max_remaining;
        decision_options.residual_dp_max_bytes = options.residual_dp_max_bytes;
        decision_options.memory_governor = options.memory_governor;
        const bool final_proof = threshold + 1 == upper;
        const bool fixed = (options.controller == ControllerMode::adaptive &&
                            options.proof_backend == ProofBackend::dfs) ||
            options.cache_mode == CacheMode::fixed_threshold ||
            (options.cache_mode == CacheMode::automatic && final_proof);
        decision_options.cache_mode = fixed ? CacheMode::fixed_threshold : CacheMode::cross_threshold;
        DecisionResult decision;
        if (options.controller == ControllerMode::adaptive &&
            options.proof_backend == ProofBackend::dfs) {
            auto found = adaptive_sessions.find(threshold);
            const bool created = found == adaptive_sessions.end();
            traced_adaptive_service = true;
            traced_created = created;
            if (created) {
                decision_options.time_limit = std::chrono::milliseconds{0};
                AdaptiveSessionEntry entry;
                bool restored_session = false;
                if (auto saved = resumed_parallel_sessions.find(threshold);
                    saved != resumed_parallel_sessions.end()) {
                    entry.quantum = saved->second.controller_quantum;
                    entry.services = saved->second.controller_services;
                    entry.generation = saved->second.session_generation;
                    entry.parallel = std::make_shared<ParallelDecisionSession>(
                        graph, saved->second, decision_options, dfs_worker_slots,
                        use_global_parallel_dfs);
                    restored_session = true;
                    resumed_parallel_sessions.erase(saved);
                } else if (auto saved = resumed_serial_sessions.find(threshold);
                           saved != resumed_serial_sessions.end()) {
                    entry.quantum = saved->second.controller_quantum;
                    entry.services = saved->second.controller_services;
                    entry.generation = saved->second.session_generation;
                    if (dfs_worker_slots > 1 || options.recursive_coarse_kernel) {
                        ParallelDecisionSnapshot forest;
                        forest.threshold = threshold;
                        forest.status = saved->second.status;
                        forest.ordering = saved->second.ordering;
                        forest.regions.push_back({1, 0, std::move(saved->second)});
                        entry.parallel = std::make_shared<ParallelDecisionSession>(
                            graph, forest, decision_options, dfs_worker_slots,
                            use_global_parallel_dfs);
                    } else {
                        entry.serial = std::make_unique<DecisionSession>(
                            graph, saved->second, decision_options);
                    }
                    restored_session = true;
                    resumed_serial_sessions.erase(saved);
                } else if (dfs_worker_slots > 1 || options.recursive_coarse_kernel)
                    entry.parallel = std::make_shared<ParallelDecisionSession>(
                        graph, threshold, decision_options, dfs_worker_slots,
                        use_global_parallel_dfs);
                else
                    entry.serial = std::make_unique<DecisionSession>(
                        graph, threshold, decision_options);
                if (entry.generation == 0) entry.generation = next_session_generation++;
                else next_session_generation = std::max(
                    next_session_generation, entry.generation + 1U);

                if (restored_session) {
                    bool has_tele = false;
                    if (resumed_checkpoint) {
                        auto tele_it = resumed_checkpoint->session_telemetry.find(threshold);
                        if (tele_it != resumed_checkpoint->session_telemetry.end() && tele_it->second.has_telemetry) {
                            entry.cumulative_nodes = tele_it->second.nodes;
                            entry.cumulative_busy_seconds = tele_it->second.busy_seconds;
                            entry.cumulative_allocated_seconds = tele_it->second.allocated_seconds;
                            entry.starvation_ticks = 0;
                            has_tele = true;
                        }
                    }
                    if (!has_tele) {
                        double orig_services = static_cast<double>(entry.services);
                        entry.cumulative_nodes = 0;
                        entry.cumulative_busy_seconds = orig_services * 0.1;
                        entry.cumulative_allocated_seconds = orig_services * 0.1;
                        entry.starvation_ticks = 0;
                        entry.services = 0;
                    }
                }
                if (progressive_sdp_session) {
                    progressive_sdp_session->activate_threshold(threshold, entry.generation);
                    std::vector<Graph::Mask> empty(graph.word_count(), 0);
                    if (!restored_session || !resumed_checkpoint ||
                        !resumed_checkpoint->progressive_sdp)
                        (void)progressive_sdp_session->enqueue({
                            {threshold, entry.generation, std::move(empty), graph.size() / 2},
                            0, lower, true});
                }
                if (progressive_cheap_bounds)
                    progressive_cheap_bounds->activate_threshold(threshold, entry.generation);
                if (entry.parallel && global_dfs_executor &&
                    entry.parallel->status() == SessionStatus::unresolved) {
                    entry.global_parallel = std::make_shared<ParallelGlobalDFSSession>(
                        entry.parallel, std::numeric_limits<std::uint64_t>::max());
                    global_dfs_executor->register_session(entry.global_parallel);
                    (void)global_dfs_executor->bind_session(entry.global_parallel, threshold);
                    // The checkpoint generation is controller-visible state;
                    // executor binding is an internal lease identity and must
                    // not rewrite a resumed session's generation.
                }
                auto inserted = adaptive_sessions.emplace(threshold, std::move(entry));
                found = inserted.first;
                if (restored_session) ++result.stats.adaptive_session_resumes;
                else ++result.stats.adaptive_sessions_created;
                traced_created = !restored_session;
            } else {
                ++result.stats.adaptive_session_resumes;
            }
            auto& entry = found->second;
            // Recurrence frequency already expresses threshold priority. A
            // second rank-based right shift erased every early quantum
            // increase for K=12/K=13 and recreated the K=15 monopoly. Preserve
            // each persistent session's own geometric service trajectory.
            const auto service_quantum = std::max<std::uint64_t>(1, entry.quantum);
            traced_quantum = service_quantum;
            traced_generation = entry.generation;
            double measured_yield_overhead = 0.0;
            if (created && entry.services == 0 && !entry.global_parallel &&
                (!entry.parallel || !use_global_parallel_dfs)) {
                const auto overhead_started = Clock::now();
                if (entry.serial) (void)entry.serial->service({1, Clock::now()});
                else (void)entry.parallel->service({1, Clock::now()});
                measured_yield_overhead = std::chrono::duration<double>(
                    Clock::now() - overhead_started).count();
                result.stats.controller_overhead_seconds += measured_yield_overhead;
            }
            auto absolute_deadline = external_deadline();
            bool milestone_boundary = false;
            if (next_milestone < options.milestones.size()) {
                const auto milestone_deadline = global_start + options.milestones[next_milestone];
                if (milestone_deadline < absolute_deadline) {
                    absolute_deadline = milestone_deadline;
                    milestone_boundary = true;
                }
            }
            const auto service_started = Clock::now();
            std::optional<std::jthread> concurrent_incumbent;
            std::exception_ptr incumbent_exception;
            const auto incumbent_calls_before = incumbent_session
                ? incumbent_session->stats().service_calls : 0U;
            const auto incumbent_candidates_before = incumbent_session
                ? incumbent_session->stats().candidate_evaluations : 0U;
            const auto incumbent_seconds_before = incumbent_session
                ? incumbent_session->stats().service_seconds : 0.0;
            if (incumbent_session && options.threads > 1 && lower + 1 < upper) {
                traced_incumbent_concurrent = true;
                concurrent_incumbent.emplace([&](std::stop_token stop) {
                    try {
                        // Admit one bounded incumbent lease before consulting
                        // the outer loop condition.  On a short, instrumented
                        // run the DFS fragment can finish before a newly
                        // created thread is scheduled; checking the deadline
                        // first then makes the advertised 3+1 split entirely
                        // unobservable.  service() itself honors the same
                        // deadline, so a late lease is a cheap censored call,
                        // never extra wall-clock work.
                        (void)incumbent_session->service(1, absolute_deadline);
                        while (!stop.stop_requested() && Clock::now() < absolute_deadline)
                            (void)incumbent_session->service(1, absolute_deadline);
                    } catch (...) { incumbent_exception = std::current_exception(); }
                });
            }
            SessionStatus event_status;
            SessionYieldReason event_reason;
            DecisionStats event_delta;
            if (entry.parallel && !entry.global_parallel &&
                entry.parallel->status() != SessionStatus::unresolved) {
                event_status = entry.parallel->status();
                event_reason = SessionYieldReason::terminal;
            } else if (entry.serial) {
                const auto event = entry.serial->service({service_quantum, absolute_deadline});
                event_status = event.status;
                event_reason = event.reason;
                event_delta = event.delta;
                traced_workers_used = 1;
            } else if (entry.global_parallel) {
                // Open all grants before draining any of them.  This is the
                // scheduler's service decision: the primary receives the
                // remaining capacity while one distinct retained forest gets
                // a concurrent lease when available.  Work units belong to
                // the adapter; these permits are dispatch capacity only.
                // Controller recurrence determines how often a forest is
                // revisited.  A global lease, however, must replay only a
                // bounded fragment so it can be stolen by another threshold.
                // Keep this independent from permit count and the geometric
                // recurrence entitlement.
                // Controller quantum determines recurrence and relative
                // service. Executor grain is a separate bounded unit: mapping
                // a new arm's quantum 1 directly to one DFS node made all but
                // one of its permits empty. A fixed minimum is independent of
                // worker count, while the cap keeps interruption replay small.
                constexpr std::uint64_t global_fragment_work_floor = 4194304;
                const auto global_fragment_work_cap = [&] {
                    constexpr std::uint64_t maximum = 33554432;
                    constexpr std::uint64_t multiplier = 32768;
                    const auto slots = static_cast<std::uint64_t>(dfs_worker_slots);
                    if (slots >= 32) return maximum;
                    return std::max(global_fragment_work_floor,
                                    multiplier * slots * slots);
                }();
                const auto bounded_fragment_work = [&](std::uint64_t quantum) {
                    const auto level = std::max<std::uint64_t>(1, quantum);
                    if (level >= global_fragment_work_cap /
                                     global_fragment_work_floor)
                        return global_fragment_work_cap;
                    return global_fragment_work_floor * level;
                };
                // Admission is centrally budgeted for this whole window.
                // A permit is a physical executor slot, not a hint: never
                // admit more auxiliary sessions than leave capacity for the
                // primary.  A claimed cheap-bound fragment removes a DFS
                // sibling, so it reserves two primary permits rather than
                // one.  Keep the selection order fixed so checkpoint/resume
                // and milestone snapshots cannot perturb service order.
                const auto window_capacity = std::max<std::size_t>(1, dfs_worker_slots);
                size_t admitted_non_dfs = 0;
                bool admit_residual = false;
                bool admit_sdp = false;
                bool admit_cheap = false;
                std::optional<ParallelUnstartedFragment> cheap_bound_fragment;

                // 1. Check residual DP
                if (use_residual_dp && residual_dp_session &&
                    residual_dp_session->applicable() &&
                    !residual_dp_session->complete() && run_residual_dp_next) {
                    if (admitted_non_dfs + 2 <= window_capacity) {
                        admit_residual = true;
                        admitted_non_dfs += 1;
                    }
                }

                // 2. Check SDP
                if (progressive_sdp_session && run_sdp_next &&
                    progressive_sdp_session->has_pending()) {
                    if (admitted_non_dfs + 2 <= window_capacity) {
                        admit_sdp = true;
                        admitted_non_dfs += 1;
                    }
                }

                // 3. Check cheap bounds
                if (progressive_cheap_bounds && run_cheap_bounds_next && cheap_bound_forest) {
                    if (admitted_non_dfs + 3 <= window_capacity) {
                        cheap_bound_fragment = cheap_bound_forest->donate_and_claim_deepest_unstarted_fragment();
                        if (!cheap_bound_fragment) {
                            cheap_bound_fragment = cheap_bound_forest->claim_deepest_unstarted_fragment();
                        }
                        if (cheap_bound_fragment) {
                            if (progressive_cheap_bounds->enqueue(*cheap_bound_fragment, cheap_bound_generation)) {
                                admit_cheap = true;
                                admitted_non_dfs += 1;
                            } else {
                                (void)cheap_bound_forest->release_claimed_unstarted_fragment(*cheap_bound_fragment);
                                cheap_bound_fragment.reset();
                                run_cheap_bounds_next = false;
                                cheap_bound_forest.reset();
                            }
                        } else {
                            run_cheap_bounds_next = false;
                            cheap_bound_forest.reset();
                        }
                    }
                }

                // Total available permits for DFS sessions:
                const auto dfs_slots_available = window_capacity - admitted_non_dfs;
                const auto minimum_primary = admit_cheap ? std::size_t{2} : std::size_t{1};

                if (dfs_slots_available < minimum_primary)
                    throw std::logic_error("global controller admission overbooked DFS window");

                // Identify unresolved retained global DFS sessions in the
                // same stable order used by the selected scheduler.
                struct SecondaryDFS {
                    AdaptiveSessionEntry* entry;
                    std::uint32_t threshold;
                };
                std::vector<std::uint32_t> pool_retained;
                {
                    auto pool_retain_live = [&](std::uint32_t c) {
                        if (c >= lower && c < upper)
                            pool_retained.push_back(c);
                    };
                    for (const auto& [c, unused] : adaptive_sessions)   { (void)unused; pool_retain_live(c); }
                    for (const auto& [c, unused] : resumed_serial_sessions)   { (void)unused; pool_retain_live(c); }
                    for (const auto& [c, unused] : resumed_parallel_sessions) { (void)unused; pool_retain_live(c); }
                }
                std::vector<std::uint32_t> pool_candidates;
                if (options.threshold_scheduler == ThresholdSchedulerMode::value_aware &&
                    value_aware_epoch.initialized()) {
                    for (const auto candidate : value_aware_epoch.candidates())
                        if (candidate >= lower && candidate < upper)
                            pool_candidates.push_back(candidate);
                } else {
                    pool_candidates = persistent_threshold_candidates(
                        lower, upper, pool_retained);
                }
                std::vector<SecondaryDFS> potential_secondaries;
                for (const auto candidate : pool_candidates) {
                    if (candidate != threshold) {
                        auto it = adaptive_sessions.find(candidate);
                        if (it != adaptive_sessions.end()) {
                            auto& other = it->second;
                            if (other.global_parallel && other.status() == SessionStatus::unresolved) {
                                potential_secondaries.push_back({&other, candidate});
                            }
                        }
                    }
                }

                // Select secondaries greedily in stable persistent-threshold-order.
                // Do not fragment a wide pool across every retained forest in
                // one window. The coarse splitter needs enough permits per
                // forest to expose a useful subtree frontier; the outer
                // recurrence already rotates all retained thresholds across
                // subsequent windows. Three concurrent DFS sessions preserve
                // a persistent primary while K=12/K=13 receive service in the
                // same window, and leave effective grain at 16/32 threads.
                constexpr std::size_t max_dfs_sessions_per_window = 3;
                std::vector<SecondaryDFS> selected_secondaries;
                for (auto& sec : potential_secondaries) {
                    size_t M_new = selected_secondaries.size() + 1;
                    if (M_new + 1 > max_dfs_sessions_per_window) break;
                    if (M_new + 1 <= dfs_slots_available) {
                        size_t permits_per_dfs = dfs_slots_available / (M_new + 1);
                        size_t primary_permits = dfs_slots_available - M_new * permits_per_dfs;
                        if (primary_permits >= minimum_primary) {
                            selected_secondaries.push_back(sec);
                            continue;
                        }
                    }
                    break;
                }

                // Calculate final permits.
                const size_t M = selected_secondaries.size();
                size_t permits_per_dfs = 0;
                size_t primary_permits = dfs_slots_available;
                if (M > 0) {
                    permits_per_dfs = dfs_slots_available / (M + 1);
                    primary_permits = dfs_slots_available - M * permits_per_dfs;
                }

                // Build leases for optional arms
                std::shared_ptr<OneShotGlobalWorkSession> residual_lease;
                std::optional<ResidualDpEvent> residual_event;
                std::uint64_t residual_lease_quantum = 0;
                if (admit_residual) {
                    constexpr std::uint64_t max_residual_quantum = 1024;
                    residual_lease_quantum = std::min(
                        residual_dp_quantum, max_residual_quantum);
                    residual_lease = std::make_shared<OneShotGlobalWorkSession>(
                        [&](Clock::time_point deadline) {
                            residual_event = residual_dp_session->service(
                                residual_lease_quantum, deadline);
                        });
                    global_dfs_executor->register_session(residual_lease);
                    (void)global_dfs_executor->bind_session(
                        residual_lease, std::numeric_limits<std::uint32_t>::max() - 1U);
                }

                std::shared_ptr<OneShotGlobalWorkSession> sdp_lease;
                std::optional<sdp::ProgressiveSdpServiceEvent> sdp_event;
                if (admit_sdp) {
                    sdp_lease = std::make_shared<OneShotGlobalWorkSession>(
                        [&](Clock::time_point deadline) {
                            sdp_event = progressive_sdp_session->service_one(deadline);
                        });
                    global_dfs_executor->register_session(sdp_lease);
                    (void)global_dfs_executor->bind_session(
                        sdp_lease, std::numeric_limits<std::uint32_t>::max() - 3U);
                }

                std::shared_ptr<OneShotGlobalWorkSession> cheap_bound_lease;
                std::optional<ProgressiveCheapBoundEvent> cheap_bound_event;
                if (admit_cheap) {
                    cheap_bound_lease = std::make_shared<OneShotGlobalWorkSession>(
                        [&](Clock::time_point) {
                            cheap_bound_event = progressive_cheap_bounds->evaluate_claimed_one(
                                *cheap_bound_fragment);
                        });
                    global_dfs_executor->register_session(cheap_bound_lease);
                    (void)global_dfs_executor->bind_session(
                        cheap_bound_lease, std::numeric_limits<std::uint32_t>::max() - 2U);
                }

                // Grant epochs
                std::vector<GlobalDFSExecutor::EpochAdmitRequest> requests;

                GlobalDFSExecutor::EpochAdmitRequest primary_req;
                primary_req.session = entry.global_parallel;
                primary_req.threshold = threshold;
                primary_req.permits = primary_permits;
                primary_req.work_units = bounded_fragment_work(service_quantum);
                requests.push_back(primary_req);

                for (auto& sec : selected_secondaries) {
                    GlobalDFSExecutor::EpochAdmitRequest sec_req;
                    sec_req.session = sec.entry->global_parallel;
                    sec_req.threshold = sec.threshold;
                    sec_req.permits = permits_per_dfs;
                    sec_req.work_units = bounded_fragment_work(
                        std::max<std::uint64_t>(1, sec.entry->quantum));
                    requests.push_back(sec_req);
                }

                if (residual_lease) {
                    GlobalDFSExecutor::EpochAdmitRequest res_req;
                    res_req.session = residual_lease;
                    res_req.threshold = std::numeric_limits<std::uint32_t>::max() - 1U;
                    res_req.permits = 1;
                    res_req.work_units = 1;
                    requests.push_back(res_req);
                }

                if (sdp_lease) {
                    GlobalDFSExecutor::EpochAdmitRequest sdp_req;
                    sdp_req.session = sdp_lease;
                    sdp_req.threshold = std::numeric_limits<std::uint32_t>::max() - 3U;
                    sdp_req.permits = 1;
                    sdp_req.work_units = 1;
                    requests.push_back(sdp_req);
                }

                if (cheap_bound_lease) {
                    GlobalDFSExecutor::EpochAdmitRequest cb_req;
                    cb_req.session = cheap_bound_lease;
                    cb_req.threshold = std::numeric_limits<std::uint32_t>::max() - 2U;
                    cb_req.permits = 1;
                    cb_req.work_units = 1;
                    requests.push_back(cb_req);
                }

                const auto admitted_ids = global_dfs_executor->grant_epochs(requests);
                std::vector<std::uint64_t> epochs_to_drain = admitted_ids;

                std::vector<std::uint64_t> secondary_epochs;
                secondary_epochs.reserve(selected_secondaries.size());
                for (std::size_t i = 0; i < selected_secondaries.size(); ++i) {
                    secondary_epochs.push_back(admitted_ids[1 + i]);
                }

                std::size_t aux_index = 1 + selected_secondaries.size();
                std::optional<std::uint64_t> residual_epoch;
                if (residual_lease) {
                    residual_epoch = admitted_ids[aux_index++];
                }

                std::optional<std::uint64_t> sdp_epoch;
                if (sdp_lease) {
                    sdp_epoch = admitted_ids[aux_index++];
                }

                std::optional<std::uint64_t> cheap_bound_epoch;
                if (cheap_bound_lease) {
                    cheap_bound_epoch = admitted_ids[aux_index++];
                }

                global_dfs_executor->drain_epochs(epochs_to_drain);
                if (residual_lease) {
                    residual_lease->rethrow_if_error();
                    global_dfs_executor->unregister_session(residual_lease);
                    run_residual_dp_next = false;
                    if (residual_event) {
                        ++result.stats.residual_dp_service_calls;
                        result.stats.residual_dp_states += residual_event->states_completed;
                        if (residual_event->complete && residual_event->exact_completion) {
                            lower = std::max(lower, *residual_event->exact_completion);
                            result.stats.residual_dp_completed = true;
                        }
                        if (!residual_event->complete &&
                            residual_dp_quantum <= std::numeric_limits<std::uint64_t>::max() / 2U)
                            residual_dp_quantum *= 2U;

                        if (trace.is_open()) {
                            trace << "{\"monotonic_milliseconds\":"
                                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                                         Clock::now() - global_start).count()
                                  << ",\"arm\":\"residual-dp\""
                                  << ",\"threshold\":" << threshold
                                  << ",\"session_generation\":0"
                                  << ",\"interval_before\":[" << interval_lower_before << ','
                                  << interval_upper_before << "]"
                                  << ",\"interval_after\":[" << lower << ',' << upper << "]"
                                  << ",\"service_quantum\":" << residual_lease_quantum
                                  << ",\"worker_allocation\":1,\"workers_used\":1"
                                  << ",\"busy_worker_seconds\":"
                                  << residual_lease->busy_worker_seconds()
                                  << ",\"allocated_worker_seconds\":"
                                  << residual_lease->allocated_worker_seconds()
                                  << ",\"event_reason\":\"residual-dp\""
                                  << ",\"switch_reason\":\"residual-dp\""
                                  << ",\"right_censored\":"
                                  << (residual_event->complete ? "false" : "true")
                                  << ",\"nodes_expanded\":0"
                                  << ",\"residual_dp_states\":" << residual_event->states_completed
                                  << ",\"residual_dp_attempts\":" << result.stats.residual_dp_attempts
                                  << ",\"residual_dp_admissions\":" << result.stats.residual_dp_admissions
                                  << ",\"residual_dp_rejections\":" << result.stats.residual_dp_governor_or_cap_rejections
                                  << ",\"residual_dp_completed_tails\":" << result.stats.residual_dp_completed_tails
                                  << ",\"residual_dp_infeasible_prunes\":" << result.stats.residual_dp_infeasible_prunes
                                  << ",\"residual_dp_feasible_witnesses\":" << result.stats.residual_dp_feasible_witnesses
                                  << ",\"residual_dp_peak_bytes\":" << result.stats.residual_dp_peak_bytes
                                  << ",\"residual_dp_seconds\":" << result.stats.residual_dp_seconds
                                  << ",\"residual_dp_cold_restarts\":" << result.stats.residual_dp_cold_restarts
                                  << "}\n";
                            trace.flush();
                        }
                    }
                }
                if (sdp_lease) {
                    sdp_lease->rethrow_if_error();
                    global_dfs_executor->unregister_session(sdp_lease);
                    run_sdp_next = false;
                    const auto event = sdp_event.value_or(sdp::ProgressiveSdpServiceEvent{});
                    if (event.committed) {
                        const auto& certified = progressive_sdp_session->certified_lower_bound();
                        if (certified) {
                            lower = std::max(lower, *certified);
                            result.stats.sdp_certified_lower_bound = certified;
                        }
                    }
                    if (trace.is_open()) {
                        const auto event_threshold = event.task ? event.task->threshold : threshold;
                        const auto event_generation = event.task ? event.task->generation : 0U;
                        trace << "{\"monotonic_milliseconds\":"
                              << std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - global_start).count()
                              << ",\"arm\":\"sdp\""
                              << ",\"threshold\":" << event_threshold
                              << ",\"session_generation\":" << event_generation
                              << ",\"interval_before\":[" << interval_lower_before << ','
                              << interval_upper_before << "]"
                              << ",\"interval_after\":[" << lower << ',' << upper << "]"
                              << ",\"service_quantum\":1"
                              << ",\"worker_allocation\":1,\"workers_used\":1"
                              << ",\"busy_worker_seconds\":"
                              << sdp_lease->busy_worker_seconds()
                              << ",\"allocated_worker_seconds\":"
                              << sdp_lease->allocated_worker_seconds()
                              << ",\"event_reason\":\"sdp\""
                              << ",\"switch_reason\":\"sdp\""
                              << ",\"right_censored\":"
                              << (event.committed ? "false" : "true")
                              << ",\"certified_contribution\":\""
                              << (event.committed ? "certified_lower_bound" : "none") << "\"}\n";
                        trace.flush();
                        if (!trace) throw std::runtime_error("cannot append strategy trace");
                    }
                }
                if (cheap_bound_lease) {
                    try {
                        cheap_bound_lease->rethrow_if_error();
                    } catch (...) {
                        (void)cheap_bound_forest->release_claimed_unstarted_fragment(
                            *cheap_bound_fragment);
                        global_dfs_executor->unregister_session(cheap_bound_lease);
                        throw;
                    }
                    auto cheap_event =
                        cheap_bound_event.value_or(ProgressiveCheapBoundEvent{});
                    (void)progressive_cheap_bounds->commit_or_release_claimed(
                        *cheap_bound_forest, *cheap_bound_fragment, cheap_event);
                    if (cheap_event.task) {
                        result.stats.partial_bounds.evaluations += cheap_event.stats.evaluations;
                        result.stats.partial_bounds.residual_degree_evaluations += cheap_event.stats.residual_degree_evaluations;
                        result.stats.partial_bounds.edge_distance_area_evaluations += cheap_event.stats.edge_distance_area_evaluations;
                        result.stats.partial_bounds.degree_distance_area_evaluations += cheap_event.stats.degree_distance_area_evaluations;
                        result.stats.partial_bounds.degeneracy_evaluations += cheap_event.stats.degeneracy_evaluations;
                        result.stats.partial_bounds.residual_degree_prunes += cheap_event.stats.residual_degree_prunes;
                        result.stats.partial_bounds.edge_distance_area_prunes += cheap_event.stats.edge_distance_area_prunes;
                        result.stats.partial_bounds.degree_distance_area_prunes += cheap_event.stats.degree_distance_area_prunes;
                        result.stats.partial_bounds.degeneracy_prunes += cheap_event.stats.degeneracy_prunes;
                        result.stats.partial_bounds.residual_degree_session_ceiling_skips += cheap_event.stats.residual_degree_session_ceiling_skips;
                        result.stats.partial_bounds.degeneracy_session_ceiling_skips += cheap_event.stats.degeneracy_session_ceiling_skips;
                        result.stats.partial_bounds.expensive_slack_gate_skips += cheap_event.stats.expensive_slack_gate_skips;

                        result.stats.partial_bounds.lagrangian_evaluations += cheap_event.stats.lagrangian_evaluations;
                        result.stats.partial_bounds.lagrangian_mincuts += cheap_event.stats.lagrangian_mincuts;
                        result.stats.partial_bounds.lagrangian_certified_prunes += cheap_event.stats.lagrangian_certified_prunes;
                        result.stats.partial_bounds.lagrangian_slack_gate_skips += cheap_event.stats.lagrangian_slack_gate_skips;
                        result.stats.partial_bounds.lagrangian_residual_gate_skips += cheap_event.stats.lagrangian_residual_gate_skips;
                        result.stats.partial_bounds.lagrangian_ineligible_gate_skips += cheap_event.stats.lagrangian_ineligible_gate_skips;
                        result.stats.partial_bounds.lagrangian_overflow_gate_skips += cheap_event.stats.lagrangian_overflow_gate_skips;
                    }
                    global_dfs_executor->unregister_session(cheap_bound_lease);
                    if (trace.is_open()) {
                        const auto event_threshold = cheap_event.task
                            ? cheap_event.task->threshold : threshold;
                        const auto event_generation = cheap_event.task
                            ? cheap_event.task->generation : 0U;
                        trace << "{\"monotonic_milliseconds\":"
                              << std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - global_start).count()
                              << ",\"arm\":\"bounds\""
                              << ",\"threshold\":" << event_threshold
                              << ",\"session_generation\":" << event_generation
                              << ",\"interval_before\":[" << interval_lower_before << ','
                              << interval_upper_before << "]"
                              << ",\"interval_after\":[" << lower << ',' << upper << "]"
                              << ",\"service_quantum\":1"
                              << ",\"worker_allocation\":1,\"workers_used\":1"
                              << ",\"busy_worker_seconds\":"
                              << cheap_bound_lease->busy_worker_seconds()
                              << ",\"allocated_worker_seconds\":"
                              << cheap_bound_lease->allocated_worker_seconds()
                              << ",\"event_reason\":\"bounds\""
                              << ",\"switch_reason\":\"bounds\""
                              << ",\"right_censored\":"
                              << (cheap_event.certified_prune ? "false" : "true")
                              << ",\"certified_contribution\":\""
                              << (cheap_event.certified_prune
                                      ? "fragment_infeasible" : "none")
                              << "\""
                              << ",\"bounds_task_present\":"
                              << (cheap_event.task ? "true" : "false")
                              << ",\"bounds_prefix_depth\":"
                              << (cheap_event.task ? cheap_event.task->prefix.size() : 0U)
                              << ",\"bounds_fragment_rejected\":"
                              << (cheap_event.fragment_rejected ? "true" : "false")
                              << ",\"bounds_stale_rejected\":"
                              << (cheap_event.stale_rejected ? "true" : "false")
                              << ",\"lagrangian_enabled\":"
                              << (options.partial_bounds.lagrangian_prefix_bound
                                      ? "true" : "false")
                              << ",\"lagrangian_evaluations\":"
                              << cheap_event.stats.lagrangian_evaluations
                              << ",\"lagrangian_slack_gate_skips\":"
                              << cheap_event.stats.lagrangian_slack_gate_skips
                              << ",\"lagrangian_residual_gate_skips\":"
                              << cheap_event.stats.lagrangian_residual_gate_skips;
                        if (cheap_event.lagrangian_evaluated) {
                            trace << ",\"lagrangian_best_bound\":" << cheap_event.lagrangian_best_bound
                                  << ",\"lagrangian_best_cardinality\":" << cheap_event.lagrangian_best_cardinality
                                  << ",\"lagrangian_best_numerator\":" << cheap_event.lagrangian_best_numerator
                                  << ",\"lagrangian_best_denominator\":" << cheap_event.lagrangian_best_denominator;
                        }
                        trace << "}\n";
                        trace.flush();
                        if (!trace) throw std::runtime_error("cannot append strategy trace");
                    }
                    run_cheap_bounds_next = false;
                    cheap_bound_forest.reset();
                }
                const auto event = entry.global_parallel->last_event();
                event_status = event.status;
                event_reason = event.reason;
                event_delta = event.delta;
                traced_workers_used = event.workers_used;
                result.stats.parallel_workers_used = std::max(
                    result.stats.parallel_workers_used, event.workers_used);
                result.stats.parallel_root_tasks_started += event.donations;
                result.stats.parallel_root_tasks_completed += event.terminal_regions;
                result.stats.allocated_worker_seconds += event.allocated_worker_seconds;
                result.stats.busy_worker_seconds += event.busy_worker_seconds;
                traced_busy_worker_seconds = event.busy_worker_seconds;
                traced_allocated_worker_seconds = event.allocated_worker_seconds;
                for (auto& sec : selected_secondaries) {
                    const auto secondary_event = sec.entry->global_parallel->last_event();
                    ++result.stats.adaptive_session_services;
                    ++result.stats.controller_events;
                    result.stats.adaptive_dfs_service_seconds +=
                        secondary_event.allocated_worker_seconds;
                    result.stats.allocated_worker_seconds += secondary_event.allocated_worker_seconds;
                    result.stats.busy_worker_seconds += secondary_event.busy_worker_seconds;
                    accumulate(result.stats, secondary_event.delta);

                    const bool sec_no_op = (secondary_event.delta.nodes_expanded == 0 && secondary_event.workers_used == 0);
                    if (!sec_no_op) {
                        sec.entry->cumulative_nodes += secondary_event.delta.nodes_expanded;
                        sec.entry->cumulative_busy_seconds += secondary_event.busy_worker_seconds;
                        sec.entry->cumulative_allocated_seconds += secondary_event.allocated_worker_seconds;
                        active_secondaries.push_back(sec.threshold);
                    }
                    sec.entry->cumulative_resolved_regions = secondary_event.delta.resolved_proof_regions_bound;
                    sec.entry->cumulative_configured_regions = secondary_event.delta.configured_proof_regions_bound;

                    const auto sec_interval_before_lower = lower;
                    const auto sec_interval_before_upper = upper;

                    if (secondary_event.status == SessionStatus::infeasible) {
                        completed_thresholds.push_back(
                            {sec.threshold, CompletedThresholdResult::infeasible});
                        lower = std::max(lower, sec.threshold + 1U);
                    } else if (secondary_event.status == SessionStatus::feasible) {
                        completed_thresholds.push_back(
                            {sec.threshold, CompletedThresholdResult::feasible});
                        if (sec.threshold < upper) {
                            upper = sec.threshold;
                            order = sec.entry->ordering();
                        }
                    }

                    const auto sec_interval_after_lower = lower;
                    const auto sec_interval_after_upper = upper;

                    if (trace.is_open()) {
                        const std::string_view sec_switch_reason = (sec.entry->services == 0) ? "session_created" : "session_resumed";
                        trace << "{\"monotonic_milliseconds\":"
                              << std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - global_start).count()
                              << ",\"arm\":\"dfs_secondary\""
                              << ",\"threshold\":" << sec.threshold
                              << ",\"session_generation\":" << sec.entry->generation
                              << ",\"interval_before\":[" << sec_interval_before_lower << ','
                              << sec_interval_before_upper << "]"
                              << ",\"interval_after\":[" << sec_interval_after_lower << ','
                              << sec_interval_after_upper << "]"
                              << ",\"service_quantum\":" << std::max<std::uint64_t>(1, sec.entry->quantum)
                              << ",\"worker_allocation\":" << permits_per_dfs
                              << ",\"workers_used\":" << secondary_event.workers_used
                              << ",\"busy_worker_seconds\":" << secondary_event.busy_worker_seconds
                              << ",\"allocated_worker_seconds\":" << secondary_event.allocated_worker_seconds
                              << ",\"event_reason\":\"" << yield_reason_name(secondary_event.reason) << "\""
                              << ",\"switch_reason\":\"" << sec_switch_reason << "\""
                              << ",\"right_censored\":" << (secondary_event.right_censored ? "true" : "false")
                              << ",\"nodes_expanded\":" << secondary_event.delta.nodes_expanded
                              << ",\"certified_contribution\":\""
                              << (secondary_event.status == SessionStatus::feasible ? "verified_incumbent" :
                                  (secondary_event.status == SessionStatus::infeasible ? "certified_lower_bound" : "none"))
                              << "\"}\n";
                        trace.flush();
                        if (!trace) throw std::runtime_error("cannot append secondary strategy trace");
                    }
                }
            } else {
                const auto event = entry.parallel->service({service_quantum, absolute_deadline});
                event_status = event.status;
                event_reason = event.reason;
                event_delta = event.delta;
                result.stats.parallel_workers_used = std::max(
                    result.stats.parallel_workers_used, event.workers_used);
                traced_workers_used = event.workers_used;
                result.stats.parallel_root_tasks_started += event.donations;
                result.stats.parallel_root_tasks_completed += event.terminal_regions;
                result.stats.allocated_worker_seconds += event.allocated_worker_seconds;
                result.stats.busy_worker_seconds += event.busy_worker_seconds;
                traced_busy_worker_seconds = event.busy_worker_seconds;
                traced_allocated_worker_seconds = event.allocated_worker_seconds;
            }
            traced_reason = event_reason;
            traced_parallel_forest = entry.parallel;
            const auto dfs_finished = Clock::now();
            if (concurrent_incumbent) {
                concurrent_incumbent->request_stop();
                concurrent_incumbent.reset();
                if (incumbent_exception) std::rethrow_exception(incumbent_exception);
                if (incumbent_session->best_width() < upper) {
                    upper = incumbent_session->best_width();
                    order = incumbent_session->best_ordering();
                    incumbent_quantum = 1;
                }
            }
            if (incumbent_session) {
                const auto& incumbent = incumbent_session->stats();
                traced_incumbent_service_calls =
                    incumbent.service_calls - incumbent_calls_before;
                traced_incumbent_candidate_evaluations =
                    incumbent.candidate_evaluations - incumbent_candidates_before;
                traced_incumbent_busy_seconds =
                    incumbent.service_seconds - incumbent_seconds_before;
            }
            const auto service_elapsed = std::chrono::duration<double>(
                dfs_finished - service_started).count();
            traced_service_seconds = service_elapsed;
            if (entry.serial) {
                traced_busy_worker_seconds = service_elapsed;
                traced_allocated_worker_seconds = service_elapsed;
                result.stats.busy_worker_seconds += service_elapsed;
                result.stats.allocated_worker_seconds += service_elapsed;
            }
            ++result.stats.adaptive_session_services;
            result.stats.adaptive_dfs_service_seconds += service_elapsed;
            decision.threshold = threshold;
            decision.stats = event_delta;
            if (event_status == SessionStatus::feasible) {
                decision.status = DecisionStatus::feasible;
                decision.ordering = entry.ordering();
            } else if (event_status == SessionStatus::infeasible) {
                decision.status = DecisionStatus::infeasible;
            } else {
                decision.status = DecisionStatus::timed_out;
            }
            if (created && measured_yield_overhead > 0.0 &&
                event_status == SessionStatus::unresolved) {
                const auto work_cost = std::max(
                    std::numeric_limits<double>::min(),
                    service_elapsed - measured_yield_overhead);
                const auto ratio = ((1.0 - options.controller_overhead_fraction) /
                    options.controller_overhead_fraction) *
                    measured_yield_overhead / work_cost;
                entry.quantum = ratio >= static_cast<double>(
                        std::numeric_limits<std::uint64_t>::max())
                    ? std::numeric_limits<std::uint64_t>::max()
                    : std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(ratio)));
            }
            // A no-progress global-parallel epoch occurs when all proof-forest
            // workers drain instantly with zero nodes expanded (all regions are
            // either waiting for external siblings, reserved by the controller,
            // or the ready queue is transiently empty).  Counting such an epoch
            // as real service inflates entry.services by tens of thousands of
            // increments per wall-clock second.  The level-synchronous scheduler
            // then sees one threshold at a vastly higher recurrence level and
            // selects it repeatedly, starving secondary thresholds and collapsing
            // worker utilization to the ~0.22 range.  Treat this as an
            // observational no-op: do not advance services or double quantum.
            // Only the global_parallel path can produce a genuinely zero-work
            // epoch; serial and !global_parallel parallel sessions always do at
            // least one DFS step before yielding.
            const bool no_progress_epoch = entry.global_parallel &&
                event_status == SessionStatus::unresolved &&
                event_delta.nodes_expanded == 0 &&
                traced_workers_used == 0;
            const bool observational_yield = event_status == SessionStatus::unresolved &&
                event_reason == SessionYieldReason::deadline && milestone_boundary &&
                Clock::now() >= absolute_deadline;
            if (observational_yield || no_progress_epoch) {
                // A milestone or no-progress epoch must not alter session order
                // or geometric service entitlement.
                if (observational_yield && adaptive_tick != 0) --adaptive_tick;
            } else {
                ++entry.services;
                if (!created &&
                    entry.quantum <= std::numeric_limits<std::uint64_t>::max() / 2U)
                    entry.quantum *= 2U;
            }

            const bool is_no_op = (event_delta.nodes_expanded == 0 && traced_workers_used == 0);
            if (!is_no_op) {
                entry.cumulative_nodes += event_delta.nodes_expanded;
                entry.cumulative_busy_seconds += traced_busy_worker_seconds;
                entry.cumulative_allocated_seconds += traced_allocated_worker_seconds;
            }
            entry.cumulative_resolved_regions = event_delta.resolved_proof_regions_bound;
            entry.cumulative_configured_regions = event_delta.configured_proof_regions_bound;

            if (options.threshold_scheduler == ThresholdSchedulerMode::value_aware) {
                if (!is_no_op) {
                    std::set<std::uint32_t> reset_thresholds;
                    reset_thresholds.insert(threshold);
                    for (const auto sec_t : active_secondaries) {
                        reset_thresholds.insert(sec_t);
                    }
                    for (auto& [t, entry_ref] : adaptive_sessions) {
                        if (reset_thresholds.count(t)) {
                            entry_ref.starvation_ticks = 0;
                        } else {
                            entry_ref.starvation_ticks += 1;
                        }
                    }
                }
            }
        } else if (options.proof_backend == ProofBackend::pb) {
            auto pb_options = options.pb_options;
            pb_options.external.time_limit = decision_options.time_limit;
            bool incremental_sat = false;
            if (pb_options.solver == pb::SolverKind::cadical &&
                (!incremental_threshold || threshold <= *incremental_threshold)) {
                result.stats.pb_incremental_attempted = true;
                if (!incremental_cadical) {
                    try {
                        incremental_encoding = pb::encode_cutwidth_threshold(
                            graph, threshold,
                            {pb_options.encoding, pb_options.break_reversal_symmetry,
                             pb_options.channel_positions});
                        incremental_cadical = std::make_unique<pb::IncrementalCadicalSession>(
                            *incremental_encoding, threshold, order, true,
                            pb_options.external.keep_temporary_files);
                        incremental_threshold = threshold;
                    } catch (const std::exception&) {
                        incremental_cadical.reset();
                        incremental_encoding.reset();
                    }
                }
                if (incremental_cadical && incremental_cadical->available()) {
                    result.stats.pb_incremental_available = true;
                    auto exploratory_budget = decision_options.time_limit;
                    if (exploratory_budget.count() != 0 &&
                        pb_options.external.proof_check_reserve < exploratory_budget)
                        exploratory_budget -= pb_options.external.proof_check_reserve;
                    const auto exploratory = incremental_cadical->solve(
                        threshold, exploratory_budget);
                    ++result.stats.pb_incremental_calls;
                    result.stats.pb_incremental_seconds += exploratory.runtime_seconds;
                    incremental_threshold = threshold;
                    if (exploratory.status == pb::IncrementalStatus::sat) {
                        try {
                            auto witness = pb::decode_ordering(
                                *incremental_encoding, exploratory.assignment);
                            if (pb::verify_ordering(graph, witness, threshold)) {
                                decision.status = DecisionStatus::feasible;
                                decision.threshold = threshold;
                                decision.ordering = std::move(witness);
                                incremental_sat = true;
                                ++result.stats.pb_incremental_sat;
                            }
                        } catch (const std::exception&) {
                        }
                    } else if (exploratory.status ==
                               pb::IncrementalStatus::unsat_exploratory) {
                        ++result.stats.pb_incremental_unsat_exploratory;
                        auto check_options = pb_options.external;
                        check_options.time_limit = remaining_time(
                            global_start, options.time_limit);
                        const auto checked = pb::check_drat_proof_external(
                            incremental_encoding->formula,
                            exploratory.added_unit_clauses,
                            exploratory.proof_path, check_options);
                        if (checked.checked) {
                            decision.status = DecisionStatus::infeasible;
                            decision.threshold = threshold;
                            incremental_sat = true; // The decision is final;
                                                    // skip external rerun.
                            result.stats.pb_last.external_status =
                                pb::ExternalSatStatus::unsat_verified;
                            result.stats.pb_last.solver_name = "cadical-incremental";
                            result.stats.pb_last.solver_version = "2.1.3";
                            result.stats.pb_last.encoding =
                                pb::encoding_name(options.pb_options.encoding);
                            result.stats.pb_last.model_hash =
                                incremental_encoding->metadata.dimacs_fnv1a64;
                            result.stats.pb_last.variables =
                                incremental_encoding->metadata.variables;
                            result.stats.pb_last.clauses =
                                incremental_encoding->metadata.clauses +
                                exploratory.added_unit_clauses.size();
                            result.stats.pb_last.proof_generated = true;
                            result.stats.pb_last.proof_checked = true;
                            result.stats.pb_last.checker_seconds =
                                checked.runtime_seconds;
                            std::error_code proof_error;
                            result.stats.pb_last.proof_bytes =
                                std::filesystem::file_size(
                                    exploratory.proof_path, proof_error);
                            if (proof_error) result.stats.pb_last.proof_bytes = 0;
                            else result.stats.pb_last.proof_hash =
                                pb::file_fnv1a64(exploratory.proof_path);
                            if (pb_options.external.keep_temporary_files)
                                result.stats.pb_last.artifact_directory =
                                    std::filesystem::path(exploratory.proof_path)
                                        .parent_path().string();
                            result.stats.pb_last.branches_completed = 1;
                            result.stats.pb_last.branches_unsat_verified = 1;
                        }
                    }
                }
            }
            pb::DecisionResult pb_result;
            if (!incremental_sat) {
                pb_options.external.time_limit = remaining_time(
                    global_start, options.time_limit);
                pb_result = pb::decide_cutwidth(
                    graph, threshold, std::move(pb_options));
                decision = pb_result.decision;
                result.stats.pb_last = pb_result.provenance;
            } else if (decision.status == DecisionStatus::feasible) {
                result.stats.pb_last.external_status = pb::ExternalSatStatus::sat;
                result.stats.pb_last.solver_name = "cadical-incremental";
                result.stats.pb_last.encoding = pb::encoding_name(options.pb_options.encoding);
                result.stats.pb_last.witness_verified = true;
            }
            ++result.stats.pb_calls;
            if (decision.status == DecisionStatus::feasible)
                ++result.stats.pb_sat_certificates;
            else if (decision.status == DecisionStatus::infeasible)
                ++result.stats.pb_unsat_certificates;
        } else if (fixed && graph.supports_mask() && options.threads == 1) {
            FixedThresholdWord64Cache cache(threshold, cache_config);
            decision = decide_cutwidth_cached(graph, threshold, decision_options, cache);
        } else if (small_cache) {
            decision = decide_cutwidth_cached(graph, threshold, decision_options, *small_cache);
        } else if (dynamic_cache) {
            decision = decide_cutwidth_cached(graph, threshold, decision_options, *dynamic_cache);
        } else if (parallel_dynamic_cache) {
            decision = decide_cutwidth_cached(
                graph, threshold, decision_options, *parallel_dynamic_cache);
        } else {
            decision = decide_cutwidth(graph, threshold, decision_options);
        }
        if (decision.status != DecisionStatus::timed_out)
            ++result.stats.decision_calls;
        ++result.stats.controller_events;
        accumulate(result.stats, decision.stats);
        if (trace.is_open() && !traced_adaptive_service) {
            const auto traced_after_lower = decision.status == DecisionStatus::infeasible
                ? (threshold == std::numeric_limits<std::uint32_t>::max()
                    ? threshold : threshold + 1U) : lower;
            const auto traced_after_upper = decision.status == DecisionStatus::feasible
                ? std::min(upper, threshold) : upper;
            trace << "{\"monotonic_milliseconds\":"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         Clock::now() - global_start).count()
                  << ",\"threshold\":" << threshold
                  << ",\"session_generation\":0"
                  << ",\"interval_before\":[" << interval_lower_before << ','
                  << interval_upper_before << ']'
                  << ",\"interval_after\":[" << traced_after_lower << ','
                  << traced_after_upper << ']'
                  << ",\"service_quantum\":0"
                  << ",\"worker_allocation\":" << options.threads
                  << ",\"event_reason\":\""
                  << (decision.status == DecisionStatus::timed_out ? "deadline" : "terminal")
                  << "\",\"switch_reason\":\"static_decision\""
                  << ",\"right_censored\":"
                  << (decision.status == DecisionStatus::timed_out ? "true" : "false")
                  << ",\"nodes_expanded\":" << decision.stats.nodes_expanded
                  << ",\"cache_lookup_probes\":" << decision.stats.cache_lookup_probes
                  << ",\"cache_insertion_probes\":"
                  << decision.stats.cache_insertion_probes << "}\n";
            trace.flush();
            if (!trace) throw std::runtime_error("cannot append static strategy trace");
        }
        if (decision.status == DecisionStatus::feasible) {
            completed_thresholds.push_back(
                {threshold, CompletedThresholdResult::feasible});
            if (threshold < upper) {
                upper = threshold;
                order = decision.ordering;
            }
            if (descending && ++descending_steps >=
                    options.descending_feasible_steps_before_binary)
                descending = false;
        } else if (decision.status == DecisionStatus::infeasible) {
            completed_thresholds.push_back(
                {threshold, CompletedThresholdResult::infeasible});
            lower = threshold + 1;
        } else {
            ++result.stats.censored_decisions;
            if (trace.is_open() && traced_adaptive_service) {
                trace << "{\"monotonic_milliseconds\":"
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             Clock::now() - global_start).count()
                      << ",\"threshold\":" << threshold
                      << ",\"session_generation\":" << traced_generation
                      << ",\"interval_before\":[" << interval_lower_before << ','
                      << interval_upper_before << "]"
                      << ",\"interval_after\":[" << lower << ',' << upper << "]"
                      << ",\"service_quantum\":" << traced_quantum
                      << ",\"worker_allocation\":" << dfs_worker_slots
                      << ",\"workers_used\":" << traced_workers_used
                      << ",\"incumbent_worker_allocation\":"
                      << (traced_incumbent_concurrent ? 1 : 0)
                      << ",\"incumbent_service_calls\":"
                      << traced_incumbent_service_calls
                      << ",\"incumbent_candidate_evaluations\":"
                      << traced_incumbent_candidate_evaluations
                      << ",\"incumbent_busy_seconds\":"
                      << traced_incumbent_busy_seconds
                      << ",\"busy_worker_seconds\":" << traced_busy_worker_seconds
                      << ",\"allocated_worker_seconds\":"
                      << traced_allocated_worker_seconds
                      << ",\"event_reason\":\"" << yield_reason_name(traced_reason) << "\""
                      << ",\"switch_reason\":\""
                      << (traced_created ? "session_created" : "session_resumed") << "\""
                      << ",\"right_censored\":true"
                      << ",\"nodes_expanded\":" << decision.stats.nodes_expanded
                      << ",\"unique_canonical_claims\":"
                      << decision.stats.unique_canonical_claims
                      << ",\"duplicate_ownership_waits\":"
                      << decision.stats.duplicate_ownership_waits
                      << ",\"cache_lookup_probes\":"
                      << decision.stats.cache_lookup_probes
                      << ",\"cache_insertion_probes\":"
                      << decision.stats.cache_insertion_probes
                      << ",\"cache_segment_growths\":"
                      << decision.stats.cache_segment_growths
                      << ",\"cache_probes_avoided_after_saturation\":"
                      << decision.stats.cache_probes_avoided_after_saturation << "}\n";
                trace.flush();
                if (!trace) throw std::runtime_error("cannot append strategy trace");
            }
            if (options.controller == ControllerMode::adaptive &&
                options.proof_backend == ProofBackend::dfs) {
                if (options.threads == 1)
                    service_incumbent(traced_service_seconds);
                // A DFS service schedules one bounded residual-DP service
                // next; the auxiliary arm itself clears this flag before it
                // returns, yielding deterministic DFS/DP interleaving.
                run_residual_dp_next = use_residual_dp;
                run_sdp_next = progressive_sdp_session && progressive_sdp_session->has_pending();
                cheap_bound_forest = traced_parallel_forest;
                cheap_bound_generation = traced_generation;
                if (progressive_cheap_bounds && cheap_bound_forest) {
                    run_cheap_bounds_next = cheap_bound_forest->inspect_deepest_unstarted_fragment().has_value();
                }
                capture_milestones();
                continue;
            }
            if (options.controller == ControllerMode::adaptive)
                threshold_portfolio.note_censored_service();
            break;
        }
        if (trace.is_open() && traced_adaptive_service) {
            trace << "{\"monotonic_milliseconds\":"
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         Clock::now() - global_start).count()
                  << ",\"threshold\":" << threshold
                  << ",\"session_generation\":" << traced_generation
                  << ",\"interval_before\":[" << interval_lower_before << ','
                  << interval_upper_before << "]"
                  << ",\"interval_after\":[" << lower << ',' << upper << "]"
                  << ",\"service_quantum\":" << traced_quantum
                  << ",\"worker_allocation\":" << dfs_worker_slots
                  << ",\"workers_used\":" << traced_workers_used
                  << ",\"incumbent_worker_allocation\":"
                  << (traced_incumbent_concurrent ? 1 : 0)
                  << ",\"incumbent_service_calls\":"
                  << traced_incumbent_service_calls
                  << ",\"incumbent_candidate_evaluations\":"
                  << traced_incumbent_candidate_evaluations
                  << ",\"incumbent_busy_seconds\":"
                  << traced_incumbent_busy_seconds
                  << ",\"busy_worker_seconds\":" << traced_busy_worker_seconds
                  << ",\"allocated_worker_seconds\":"
                  << traced_allocated_worker_seconds
                  << ",\"event_reason\":\"" << yield_reason_name(traced_reason) << "\""
                  << ",\"switch_reason\":\""
                  << (traced_created ? "session_created" : "session_resumed") << "\""
                  << ",\"right_censored\":false"
                  << ",\"certified_contribution\":\""
                  << (decision.status == DecisionStatus::feasible ? "verified_incumbent" :
                      "certified_lower_bound") << "\""
                  << ",\"nodes_expanded\":" << decision.stats.nodes_expanded
                  << ",\"unique_canonical_claims\":"
                  << decision.stats.unique_canonical_claims
                  << ",\"duplicate_ownership_waits\":"
                  << decision.stats.duplicate_ownership_waits
                  << ",\"cache_lookup_probes\":"
                  << decision.stats.cache_lookup_probes
                  << ",\"cache_insertion_probes\":"
                  << decision.stats.cache_insertion_probes
                  << ",\"cache_segment_growths\":"
                  << decision.stats.cache_segment_growths
                  << ",\"cache_probes_avoided_after_saturation\":"
                  << decision.stats.cache_probes_avoided_after_saturation << "}\n";
            trace.flush();
            if (!trace) throw std::runtime_error("cannot append strategy trace");
        }
        if (options.threads == 1)
            service_incumbent(traced_service_seconds);
        capture_milestones();
    }
    result.lower_bound = lower;
    result.upper_bound = upper;
    result.ordering = order;
    result.optimal = lower == upper;
    if (incumbent_session) {
        const auto& incumbent = incumbent_session->stats();
        result.stats.incumbent_service_calls = incumbent.service_calls;
        result.stats.incumbent_iterations = incumbent.iterations;
        result.stats.incumbent_candidate_evaluations = incumbent.candidate_evaluations;
        result.stats.incumbent_verified_improvements = incumbent.verified_improvements;
        result.stats.incumbent_no_progress_bursts = incumbent.no_progress_bursts;
        result.stats.incumbent_service_seconds = incumbent.service_seconds;
    }
    if (options.memory_governor) {
        (void)options.memory_governor->sample_rss();
        result.stats.memory = options.memory_governor->stats();
    }
    capture_milestones();
    result.stats.busy_worker_seconds += result.stats.incumbent_service_seconds;
    if (global_dfs_executor) {
        auto telemetry = global_dfs_executor->get_telemetry();
        result.stats.peak_active_physical_leases = telemetry.peak_active_leases;
        result.stats.useful_leases = telemetry.useful_leases;
        result.stats.empty_claim_exits = telemetry.empty_claim_exits;
        result.stats.cross_session_steals = telemetry.cross_session_steals;
        result.stats.per_epoch_useful_work = telemetry.per_epoch_useful_work;
        result.stats.per_threshold_useful_work = telemetry.per_threshold_useful_work;
        result.stats.allocated_worker_seconds =
            global_dfs_executor->allocated_worker_seconds() +
            result.stats.incumbent_service_seconds;
        result.stats.busy_worker_seconds =
            global_dfs_executor->busy_worker_seconds() +
            result.stats.incumbent_service_seconds;
    }
    result.stats.compatibility_wall_time_capacity_seconds = std::chrono::duration<double>(
        Clock::now() - global_start).count() * static_cast<double>(options.threads);
    if (state_oracle) {
        const auto oracle_stats = state_oracle->stats();
        result.stats.sdp_state_requests = oracle_stats.requests;
        result.stats.sdp_state_certified = oracle_stats.certified;
        result.stats.sdp_state_cache_hits = oracle_stats.cache_hits;
        result.stats.sdp_state_calls = oracle_stats.calls;
        result.stats.sdp_state_busy = oracle_stats.busy;
        result.stats.sdp_state_budget_rejections = oracle_stats.budget_rejections;
        result.stats.sdp_state_uncertified = oracle_stats.uncertified;
        result.stats.sdp_state_dimension_rejections = oracle_stats.dimension_rejections;
        result.stats.sdp_state_preferred_max_dimension = oracle_stats.preferred_max_dimension;
        result.stats.sdp_solve_seconds += oracle_stats.backend_seconds;
        result.stats.sdp_attempted = oracle_stats.calls != 0;
#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
        result.stats.sdp_available = true;
#endif
    }
    if (pb_sat_root_thread) {
        pb_sat_root_thread->request_stop();
        pb_sat_root_thread.reset();
        result.stats.pb_sat_root_solver_seconds += active_pb_sat_root_job->solver_seconds;
        result.stats.pb_sat_root_checker_seconds += active_pb_sat_root_job->checker_seconds;
        result.stats.pb_sat_root_active_threshold = active_pb_sat_root_job->threshold;
        result.stats.pb_sat_root_active_cardinality = active_pb_sat_root_job->cardinality;
        result.stats.pb_sat_root_last_cnf_path = active_pb_sat_root_job->cnf_path;
        result.stats.pb_sat_root_last_proof_path = active_pb_sat_root_job->proof_path;
        result.stats.pb_sat_root_last_result = "CANCELLED";
        ++result.stats.pb_sat_root_failures;
    }
    const auto pb_sat_root_busy_seconds = result.stats.pb_sat_root_solver_seconds +
        result.stats.pb_sat_root_checker_seconds;
    result.stats.allocated_worker_seconds += pb_sat_root_busy_seconds;
    result.stats.busy_worker_seconds += pb_sat_root_busy_seconds;
    write_live_checkpoint();
    return result;
}

} // namespace

CheckpointCompatibility adaptive_checkpoint_compatibility(
    const Graph& graph, const OptimizerV2Options& options) {
    std::ostringstream graph_encoding;
    graph_encoding << "cutwidth-graph-v1\n" << graph.size() << '\n';
    for (Graph::Vertex u = 0; u < graph.size(); ++u)
        for (Graph::Vertex v = u + 1; v < graph.size(); ++v)
            if (graph.adjacent(u, v)) graph_encoding << u << ',' << v << '\n';

    std::ostringstream proof;
    proof << "proof-policy-v2"
          << ";twins=" << options.use_twin_symmetry
          << ";lookahead=" << options.use_depth_two_lookahead
          << ";lookahead_gate=" << options.depth_two_lookahead_max_remaining
          << ";partial=" << options.use_partial_bounds
          << ";residual_degree=" << options.partial_bounds.residual_degree
          << ";edge_area=" << options.partial_bounds.edge_distance_area
          << ";degree_area=" << options.partial_bounds.degree_distance_area
          << ";degeneracy=" << options.partial_bounds.degeneracy
          << ";expensive_bound_slack="
          << options.partial_bounds.expensive_max_slack
          << ";lagrangian_prefix_bound=" << options.partial_bounds.lagrangian_prefix_bound
          << ";lagrangian_max_slack=" << options.partial_bounds.lagrangian_max_slack
          << ";lagrangian_max_residual=" << options.partial_bounds.lagrangian_max_residual
          << ";lagrangian_denominator=" << options.partial_bounds.lagrangian_denominator
          << ";memo_depth=" << static_cast<unsigned>(options.node_memo_depth)
          << ";memo_gate=" << options.node_memo_max_remaining
          << ";dfs_residual_dp_gate=" << options.dfs_residual_dp_max_remaining
          << ";residual_dp_max_bytes=" << options.residual_dp_max_bytes;

    std::ostringstream order;
    order << "candidate-order-v2;node_order=" << static_cast<int>(options.node_order)
          << ";node_state=" << static_cast<int>(options.node_state)
          << ";fail_first=1;twin_lowest_first=1";
    return {
        sha256_hex(graph_encoding.str()),
        sha256_hex("cutwidth-adaptive-semantic-v2;cwcp2-region-forest-v1"),
        sha256_hex(proof.str()),
        sha256_hex(order.str())};
}

DecisionResult decide_cutwidth_v2(const Graph& graph, std::uint32_t threshold,
                                  DecisionOptions options) {
    const auto start = Clock::now();
    const auto overall_limit = options.time_limit;
    DecisionResult combined;
    combined.threshold = threshold;
    combined.status = DecisionStatus::feasible;
    combined.ordering.reserve(graph.size());
    for (const auto& component : connected_components(graph)) {
        options.time_limit = remaining_time(start, overall_limit);
        const auto part = decide_cutwidth(component.graph, threshold, options);
        combined.stats.nodes_expanded += part.stats.nodes_expanded;
        combined.stats.parallel_workers_used = std::max(
            combined.stats.parallel_workers_used, part.stats.parallel_workers_used);
        combined.stats.parallel_root_tasks_started +=
            part.stats.parallel_root_tasks_started;
        combined.stats.parallel_root_tasks_completed +=
            part.stats.parallel_root_tasks_completed;
        combined.stats.children_rejected_by_cut += part.stats.children_rejected_by_cut;
        combined.stats.failed_cache_hits += part.stats.failed_cache_hits;
        combined.stats.failed_cache_queries += part.stats.failed_cache_queries;
        combined.stats.failed_states_recorded += part.stats.failed_states_recorded;
        combined.stats.unique_canonical_claims += part.stats.unique_canonical_claims;
        combined.stats.duplicate_ownership_waits += part.stats.duplicate_ownership_waits;
        combined.stats.ownership_saturation += part.stats.ownership_saturation;
        combined.stats.twin_symmetric_children_skipped += part.stats.twin_symmetric_children_skipped;
        combined.stats.depth_two_lookahead_checks += part.stats.depth_two_lookahead_checks;
        combined.stats.children_rejected_by_depth_two_lookahead +=
            part.stats.children_rejected_by_depth_two_lookahead;
        for (std::size_t d = 0; d < combined.stats.node_memo_hits_by_depth.size(); ++d)
            combined.stats.node_memo_hits_by_depth[d] += part.stats.node_memo_hits_by_depth[d];
        combined.stats.node_memo_computations += part.stats.node_memo_computations;
        combined.stats.node_memo_prunes += part.stats.node_memo_prunes;
        combined.stats.node_memo_child_rejections += part.stats.node_memo_child_rejections;
        combined.stats.node_memo_collisions += part.stats.node_memo_collisions;
        combined.stats.node_memo_saturation += part.stats.node_memo_saturation;
        combined.stats.node_memo_memory_bytes = std::max(
            combined.stats.node_memo_memory_bytes, part.stats.node_memo_memory_bytes);
        combined.stats.node_memo_available = combined.stats.node_memo_available ||
            part.stats.node_memo_available;
        combined.stats.node_state_updates += part.stats.node_state_updates;
        combined.stats.residual_histogram_updates += part.stats.residual_histogram_updates;
        combined.stats.node_sorts_avoided += part.stats.node_sorts_avoided;
        combined.stats.failed_state_bounds_strengthened += part.stats.failed_state_bounds_strengthened;
        combined.stats.failed_state_insertions_skipped += part.stats.failed_state_insertions_skipped;
        combined.stats.cache_collisions += part.stats.cache_collisions;
        combined.stats.cache_segment_growths += part.stats.cache_segment_growths;
        combined.stats.cache_lookup_probes += part.stats.cache_lookup_probes;
        combined.stats.cache_insertion_probes += part.stats.cache_insertion_probes;
        combined.stats.cache_probes_avoided_after_saturation +=
            part.stats.cache_probes_avoided_after_saturation;
        combined.stats.cache_page_promotions += part.stats.cache_page_promotions;
        combined.stats.cache_page_second_chances += part.stats.cache_page_second_chances;
        combined.stats.cache_pages_recycled += part.stats.cache_pages_recycled;
        combined.stats.cache_replacement_admissions += part.stats.cache_replacement_admissions;
        combined.stats.cache_entries_evicted += part.stats.cache_entries_evicted;
        combined.stats.cache_evicted_depth_sum += part.stats.cache_evicted_depth_sum;
        combined.stats.cache_maximum_evicted_depth = std::max(
            combined.stats.cache_maximum_evicted_depth,
            part.stats.cache_maximum_evicted_depth);
        combined.stats.failed_state_cache_size = std::max(
            combined.stats.failed_state_cache_size, part.stats.failed_state_cache_size);
        combined.stats.failed_state_cache_capacity = std::max(
            combined.stats.failed_state_cache_capacity, part.stats.failed_state_cache_capacity);
        combined.stats.failed_state_cache_memory_bytes = std::max(
            combined.stats.failed_state_cache_memory_bytes, part.stats.failed_state_cache_memory_bytes);
        combined.stats.partial_bounds.evaluations += part.stats.partial_bounds.evaluations;
        combined.stats.partial_bounds.residual_degree_prunes += part.stats.partial_bounds.residual_degree_prunes;
        combined.stats.partial_bounds.edge_distance_area_prunes += part.stats.partial_bounds.edge_distance_area_prunes;
        combined.stats.partial_bounds.degree_distance_area_prunes += part.stats.partial_bounds.degree_distance_area_prunes;
        combined.stats.partial_bounds.degeneracy_prunes += part.stats.partial_bounds.degeneracy_prunes;
        combined.stats.configured_proof_regions_bound = std::max(
            combined.stats.configured_proof_regions_bound, part.stats.configured_proof_regions_bound);
        combined.stats.resolved_proof_regions_bound = std::max(
            combined.stats.resolved_proof_regions_bound, part.stats.resolved_proof_regions_bound);
        combined.stats.peak_proof_regions = std::max(
            combined.stats.peak_proof_regions, part.stats.peak_proof_regions);
        combined.stats.suppressed_donations += part.stats.suppressed_donations;
        combined.stats.residual_dp_attempts += part.stats.residual_dp_attempts;
        combined.stats.residual_dp_admissions += part.stats.residual_dp_admissions;
        combined.stats.residual_dp_governor_or_cap_rejections += part.stats.residual_dp_governor_or_cap_rejections;
        combined.stats.residual_dp_completed_tails += part.stats.residual_dp_completed_tails;
        combined.stats.residual_dp_infeasible_prunes += part.stats.residual_dp_infeasible_prunes;
        combined.stats.residual_dp_feasible_witnesses += part.stats.residual_dp_feasible_witnesses;
        combined.stats.residual_dp_states += part.stats.residual_dp_states;
        combined.stats.residual_dp_peak_bytes = std::max(
            combined.stats.residual_dp_peak_bytes, part.stats.residual_dp_peak_bytes);
        combined.stats.residual_dp_seconds += part.stats.residual_dp_seconds;
        combined.stats.residual_dp_cold_restarts += part.stats.residual_dp_cold_restarts;
        if (part.status != DecisionStatus::feasible) {
            combined.status = part.status;
            combined.ordering.clear();
            return combined;
        }
        for (const auto local : part.ordering)
            combined.ordering.push_back(component.parent_vertices[local]);
    }
    return combined;
}

OptimizerV2Result optimize_cutwidth_v2(const Graph& graph, OptimizerV2Options options) {
    if (options.threads == 0) throw std::invalid_argument("thread count must be positive");
#ifndef CUTWIDTH_HAVE_ONETBB
    if (options.parallel_runtime == ParallelRuntime::onetbb)
        throw std::invalid_argument(
            "oneTBB parallel runtime requested but this build has no oneTBB support");
#endif
    if (!(options.controller_overhead_fraction > 0.0 &&
          options.controller_overhead_fraction < 1.0))
        throw std::invalid_argument("controller overhead fraction must be between 0 and 1");
    if (!std::is_sorted(options.milestones.begin(), options.milestones.end()) ||
        std::adjacent_find(options.milestones.begin(), options.milestones.end()) !=
            options.milestones.end())
        throw std::invalid_argument("milestones must be strictly increasing");
    if (std::any_of(options.milestones.begin(), options.milestones.end(),
                    [](auto value) { return value.count() <= 0; }))
        throw std::invalid_argument("milestones must be positive");
    if (options.memory_budget_bytes != 0 &&
        options.failed_state_cache_memory_bytes > options.memory_budget_bytes)
        throw std::invalid_argument("cache memory exceeds global memory budget");
    if (options.controller == ControllerMode::adaptive && options.adaptive_arms.empty())
        throw std::invalid_argument("adaptive controller requires at least one arm");
    if (options.controller == ControllerMode::adaptive &&
        options.cache_mode == CacheMode::cross_threshold)
        throw std::invalid_argument(
            "adaptive controller requires auto or fixed-threshold cache mode");
    if (options.controller == ControllerMode::adaptive &&
        std::find(options.adaptive_arms.begin(), options.adaptive_arms.end(), "dfs") ==
            options.adaptive_arms.end())
        throw std::invalid_argument(
            "adaptive exact controller requires the dfs arm; ablations may disable optional arms");
    if (options.controller != ControllerMode::adaptive &&
        (options.checkpoint_out || options.resume))
        throw std::invalid_argument(
            "checkpoint and resume require the adaptive controller");
    if (!options.memory_governor)
        options.memory_governor = std::make_shared<MemoryGovernor>(options.memory_budget_bytes);
    const auto start = Clock::now();
    OptimizerV2Result combined;
    std::optional<MilpResult> milp;
    std::optional<std::pair<std::uint32_t, std::vector<Vertex>>> root_seed;
    std::optional<std::uint32_t> sdp_root_bound;
    if (options.sdp_iterations > 0 && graph.size() != 0) {
        combined.stats.sdp_attempted = true;
#ifdef CUTWIDTH_ENABLE_SDP_PROTOTYPE
        combined.stats.sdp_available = true;
        // Establish a verified incumbent before any root oracle can consume
        // the global deadline. Connected search reuses this exact seed.
        root_seed = heuristic(
            graph, start, options.time_limit, options, &combined.stats);
        if (options.sdp_backend == SdpBackend::clarabel_bisection) {
#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
            if (graph.size() <= options.sdp_max_dimension) {
                const auto sdp_started = Clock::now();
                std::vector<std::size_t> cardinalities;
                const std::size_t half = graph.size() / 2;
                for (const auto offset : options.sdp_bisection_offsets) {
                    if (offset > half) continue;
                    const auto k = half - offset;
                    if (k == 0 || k >= graph.size()) continue;
                    if (std::find(cardinalities.begin(), cardinalities.end(), k) == cardinalities.end())
                        cardinalities.push_back(k);
                }
                for (std::size_t call = 0; call < cardinalities.size(); ++call) {
                    double remaining = std::numeric_limits<double>::infinity();
                    if (options.sdp_time_seconds > 0.0) {
                        remaining = std::min(remaining, std::max(0.0,
                            options.sdp_time_seconds -
                            std::chrono::duration<double>(Clock::now() - sdp_started).count()));
                    }
                    if (options.time_limit.count() > 0) {
                        remaining = std::min(remaining, std::max(0.0,
                            static_cast<double>(options.time_limit.count()) / 1000.0 -
                            std::chrono::duration<double>(Clock::now() - start).count()));
                    }
                    if (remaining <= 0.0) break;

                    sdp::ClarabelBisectionOptions clarabel;
                    clarabel.cardinality = cardinalities[call];
                    clarabel.max_iterations = options.sdp_iterations;
                    clarabel.quantization_bits = options.sdp_quantization_bits;
                    clarabel.triangle_cut_limit = options.sdp_triangle_cuts;
                    if (std::isfinite(remaining)) {
                        // Share the still-global budget so an early cardinality
                        // cannot starve every later scheduled certificate.
                        clarabel.time_limit_seconds = remaining /
                            static_cast<double>(cardinalities.size() - call);
                    }
                    const auto solved = sdp::solve_bisection_sdp_clarabel(graph, clarabel);
                    ++combined.stats.sdp_bisection_calls;
                    combined.stats.sdp_solver_status = static_cast<int>(solved.status);
                    combined.stats.sdp_dual_objective = std::max(
                        combined.stats.sdp_dual_objective, solved.raw_dual_bound);
                    combined.stats.sdp_primal_residual = solved.primal_residual;
                    combined.stats.sdp_dual_residual = solved.dual_residual;
                    combined.stats.sdp_solve_seconds += solved.solve_seconds;
                    combined.stats.sdp_solver_iterations += solved.iterations;
                    combined.stats.sdp_triangle_cuts += solved.triangle_cuts;
                    combined.stats.sdp_raw_converged =
                        combined.stats.sdp_raw_converged ||
                        solved.status == sdp::ClarabelSdpStatus::solved;
                    // Never turn the floating-point objective into a pruning
                    // bound. Only the independently verified exact certificate
                    // may strengthen the root lower bound.
                    if (solved.certificate.valid &&
                        solved.certificate.integer_lower_bound) {
                        const auto bound = *solved.certificate.integer_lower_bound;
                        if (!sdp_root_bound || bound > *sdp_root_bound) sdp_root_bound = bound;
                    }
                }
                combined.stats.sdp_certified_lower_bound = sdp_root_bound;
            } else {
                combined.stats.sdp_solver_status = 7;
            }
#else
            combined.stats.sdp_available = false;
#endif
        } else {
        sdp::CutwidthSdpOperator op(graph);
        if (op.dimension() <= options.sdp_max_dimension) {
            sdp::SdpCertificate certificate;
            if (options.sdp_backend == SdpBackend::clarabel) {
#ifdef CUTWIDTH_HAVE_CLARABEL_SDP
                sdp::ClarabelSdpOptions clarabel;
                clarabel.max_dimension = options.sdp_max_dimension;
                clarabel.max_iterations = options.sdp_iterations;
                clarabel.max_psd_triangle_entries = options.sdp_max_cone_entries;
                clarabel.time_limit_seconds = options.sdp_time_seconds;
                if (options.time_limit.count() > 0) {
                    const double global_remaining = std::max(0.001,
                        static_cast<double>(options.time_limit.count()) / 1000.0 -
                        std::chrono::duration<double>(Clock::now() - start).count());
                    clarabel.time_limit_seconds = clarabel.time_limit_seconds > 0
                        ? std::min(clarabel.time_limit_seconds, global_remaining)
                        : global_remaining;
                }
                const auto solved = sdp::solve_basic_sdp_clarabel(op, clarabel);
                combined.stats.sdp_solver_status = static_cast<int>(solved.status);
                combined.stats.sdp_primal_objective = solved.primal_objective;
                combined.stats.sdp_dual_objective = solved.dual_objective;
                combined.stats.sdp_primal_residual = solved.primal_residual;
                combined.stats.sdp_dual_residual = solved.dual_residual;
                combined.stats.sdp_solve_seconds = solved.solve_seconds;
                combined.stats.sdp_solver_iterations = solved.iterations;
                combined.stats.sdp_raw_converged =
                    solved.status == sdp::ClarabelSdpStatus::solved;
                certificate = solved.certificate;
#else
                combined.stats.sdp_available = false;
#endif
            } else {
                sdp::DenseAdmmOptions admm;
                admm.iterations = options.sdp_iterations;
                admm.constraint_projection_sweeps = options.sdp_projection_sweeps;
                admm.max_dimension = options.sdp_max_dimension;
                admm.alpha = root_seed->first;
                const auto raw = sdp::solve_dense_feasibility_admm(op, admm);
                combined.stats.sdp_raw_converged = raw.converged;
                combined.stats.sdp_primal_residual = raw.primal_residual;
                sdp::BasicDualCandidate dual;
                dual.diagonal_multipliers.assign(op.dimension() - 1, 0.0);
                dual.cut_weights.assign(graph.size(), 1.0);
                certificate = sdp::recover_basic_certificate(
                    op, dual, {options.sdp_max_dimension});
            }
            if (certificate.valid) {
                combined.stats.sdp_certified_lower_bound = certificate.integer_lower_bound;
                sdp_root_bound = certificate.integer_lower_bound;
            }
        } else if (options.sdp_backend == SdpBackend::clarabel) {
            combined.stats.sdp_solver_status = 7; // ClarabelSdpStatus::unsupported
        }
        }
#endif
    }
    if (options.milp_time_seconds > 0.0 && graph.size() != 0) {
        // Preserve the normal high-quality incumbent before spending the
        // remaining root budget on MILP certification.
        if (!root_seed)
            root_seed = heuristic(
                graph, start, options.time_limit, options, &combined.stats);
        double budget = options.milp_time_seconds;
        if (options.time_limit.count() > 0) {
            const double used = std::chrono::duration<double>(Clock::now() - start).count();
            budget = std::min(budget,
                std::max(0.001, static_cast<double>(options.time_limit.count()) / 1000.0 - used));
        }
        const auto milp_started = Clock::now();
        milp = run_highs(graph, {}, budget);
        combined.stats.milp_attempted = true;
        combined.stats.milp_status = milp->status;
        combined.stats.milp_runtime_seconds =
            std::chrono::duration<double>(Clock::now() - milp_started).count();
        combined.stats.milp_model_build_seconds = milp->model_build_seconds;
        combined.stats.milp_solve_seconds = milp->solve_seconds;
        combined.stats.milp_nodes = milp->mip_nodes;
        combined.stats.milp_diagnostic_dual_bound = milp->diagnostic_dual_bound;
        if (milp->status == MilpStatus::optimal && milp->optimum &&
            milp->incumbent_width == milp->optimum &&
            graph.validate_ordering(milp->ordering)) {
            combined.optimal = true;
            combined.lower_bound = *milp->optimum;
            combined.upper_bound = *milp->optimum;
            combined.ordering = std::move(milp->ordering);
            combined.stats.milp_incumbent_accepted = true;
            return combined;
        }
    }
    combined.optimal = true;
    combined.ordering.reserve(graph.size());
    auto components = [&] {
        if (options.controller != ControllerMode::adaptive)
            return connected_components(graph);
        std::vector<Vertex> identity(graph.size());
        std::iota(identity.begin(), identity.end(), Vertex{0});
        VertexSet full(graph.size(), true);
        std::vector<InducedComponent> whole;
        whole.push_back({graph, std::move(identity), std::move(full)});
        return whole;
    }();
    for (const auto& component : components) {
        const auto* seed = root_seed && components.size() == 1 ? &*root_seed : nullptr;
        const auto part = optimize_connected(component.graph, options, start, seed);
        combined.optimal &= part.optimal;
        combined.lower_bound = std::max(combined.lower_bound, part.lower_bound);
        combined.upper_bound = std::max(combined.upper_bound, part.upper_bound);
        combined.stats.decision_calls += part.stats.decision_calls;
        combined.stats.controller_events += part.stats.controller_events;
        combined.stats.censored_decisions += part.stats.censored_decisions;
        combined.stats.controller_overhead_seconds +=
            part.stats.controller_overhead_seconds;
        combined.stats.adaptive_sessions_created += part.stats.adaptive_sessions_created;
        combined.stats.adaptive_session_resumes += part.stats.adaptive_session_resumes;
        combined.stats.adaptive_session_services += part.stats.adaptive_session_services;
        combined.stats.adaptive_dfs_service_seconds += part.stats.adaptive_dfs_service_seconds;
        combined.stats.adaptive_dfs_worker_allocation = std::max(
            combined.stats.adaptive_dfs_worker_allocation,
            part.stats.adaptive_dfs_worker_allocation);
        combined.stats.adaptive_incumbent_worker_allocation = std::max(
            combined.stats.adaptive_incumbent_worker_allocation,
            part.stats.adaptive_incumbent_worker_allocation);
        combined.stats.allocated_worker_seconds += part.stats.allocated_worker_seconds;
        combined.stats.busy_worker_seconds += part.stats.busy_worker_seconds;
        combined.stats.compatibility_wall_time_capacity_seconds +=
            part.stats.compatibility_wall_time_capacity_seconds;
        combined.stats.peak_active_physical_leases = std::max(
            combined.stats.peak_active_physical_leases,
            part.stats.peak_active_physical_leases);
        combined.stats.useful_leases += part.stats.useful_leases;
        combined.stats.empty_claim_exits += part.stats.empty_claim_exits;
        combined.stats.cross_session_steals += part.stats.cross_session_steals;
        for (const auto& [epoch, work] : part.stats.per_epoch_useful_work)
            combined.stats.per_epoch_useful_work[epoch] += work;
        for (const auto& [threshold, work] : part.stats.per_threshold_useful_work)
            combined.stats.per_threshold_useful_work[threshold] += work;
        combined.stats.residual_dp_service_calls += part.stats.residual_dp_service_calls;
        combined.stats.residual_dp_states += part.stats.residual_dp_states;
        combined.stats.residual_dp_projected_bytes = std::max(
            combined.stats.residual_dp_projected_bytes,
            part.stats.residual_dp_projected_bytes);
        combined.stats.residual_dp_applicable = combined.stats.residual_dp_applicable ||
            part.stats.residual_dp_applicable;
        combined.stats.residual_dp_admitted = combined.stats.residual_dp_admitted ||
            part.stats.residual_dp_admitted;
        if (combined.stats.residual_dp_skip_reason.empty())
            combined.stats.residual_dp_skip_reason = part.stats.residual_dp_skip_reason;
        combined.stats.residual_dp_completed = combined.stats.residual_dp_completed ||
            part.stats.residual_dp_completed;
        combined.stats.memory = part.stats.memory;
        combined.stats.incumbent_service_calls += part.stats.incumbent_service_calls;
        combined.stats.incumbent_iterations += part.stats.incumbent_iterations;
        combined.stats.incumbent_candidate_evaluations +=
            part.stats.incumbent_candidate_evaluations;
        combined.stats.incumbent_verified_improvements +=
            part.stats.incumbent_verified_improvements;
        combined.stats.incumbent_no_progress_bursts +=
            part.stats.incumbent_no_progress_bursts;
        combined.stats.incumbent_service_seconds += part.stats.incumbent_service_seconds;
        if (components.size() == 1) combined.stats.milestones = part.stats.milestones;
        combined.stats.parallel_workers_used = std::max(
            combined.stats.parallel_workers_used, part.stats.parallel_workers_used);
        combined.stats.parallel_root_tasks_started +=
            part.stats.parallel_root_tasks_started;
        combined.stats.parallel_root_tasks_completed +=
            part.stats.parallel_root_tasks_completed;
        combined.stats.root_degree_bound = std::max(
            combined.stats.root_degree_bound, part.stats.root_degree_bound);
        combined.stats.root_density_bound = std::max(
            combined.stats.root_density_bound, part.stats.root_density_bound);
        combined.stats.root_average_degree_bound = std::max(
            combined.stats.root_average_degree_bound,
            part.stats.root_average_degree_bound);
        combined.stats.root_grooming_bound = std::max(
            combined.stats.root_grooming_bound, part.stats.root_grooming_bound);
        combined.stats.nodes_expanded += part.stats.nodes_expanded;
        combined.stats.children_rejected_by_cut += part.stats.children_rejected_by_cut;
        combined.stats.failed_cache_hits += part.stats.failed_cache_hits;
        combined.stats.failed_states_recorded += part.stats.failed_states_recorded;
        combined.stats.twin_symmetric_children_skipped += part.stats.twin_symmetric_children_skipped;
        combined.stats.depth_two_lookahead_checks += part.stats.depth_two_lookahead_checks;
        combined.stats.children_rejected_by_depth_two_lookahead +=
            part.stats.children_rejected_by_depth_two_lookahead;
        combined.stats.cache_strengthenings += part.stats.cache_strengthenings;
        combined.stats.cache_insertions_skipped += part.stats.cache_insertions_skipped;
        combined.stats.cache_collisions += part.stats.cache_collisions;
        combined.stats.cache_segment_growths += part.stats.cache_segment_growths;
        combined.stats.cache_lookup_probes += part.stats.cache_lookup_probes;
        combined.stats.cache_insertion_probes += part.stats.cache_insertion_probes;
        combined.stats.cache_probes_avoided_after_saturation +=
            part.stats.cache_probes_avoided_after_saturation;
        combined.stats.cache_page_promotions += part.stats.cache_page_promotions;
        combined.stats.cache_page_second_chances += part.stats.cache_page_second_chances;
        combined.stats.cache_pages_recycled += part.stats.cache_pages_recycled;
        combined.stats.cache_replacement_admissions += part.stats.cache_replacement_admissions;
        combined.stats.cache_entries_evicted += part.stats.cache_entries_evicted;
        combined.stats.cache_evicted_depth_sum += part.stats.cache_evicted_depth_sum;
        combined.stats.cache_maximum_evicted_depth = std::max(
            combined.stats.cache_maximum_evicted_depth,
            part.stats.cache_maximum_evicted_depth);
        combined.stats.unique_canonical_claims += part.stats.unique_canonical_claims;
        combined.stats.duplicate_ownership_waits += part.stats.duplicate_ownership_waits;
        combined.stats.ownership_saturation += part.stats.ownership_saturation;
        combined.stats.resumed_from_checkpoint = combined.stats.resumed_from_checkpoint ||
            part.stats.resumed_from_checkpoint;
        combined.stats.checkpoint_elapsed_milliseconds = std::max(
            combined.stats.checkpoint_elapsed_milliseconds,
            part.stats.checkpoint_elapsed_milliseconds);
        combined.stats.checkpoints_written += part.stats.checkpoints_written;
        combined.stats.checkpoint_write_seconds += part.stats.checkpoint_write_seconds;
        combined.stats.checkpoint_reserve_milliseconds = std::max(
            combined.stats.checkpoint_reserve_milliseconds,
            part.stats.checkpoint_reserve_milliseconds);
        combined.stats.cache_peak_entries = std::max(
            combined.stats.cache_peak_entries, part.stats.cache_peak_entries);
        combined.stats.cache_peak_capacity = std::max(
            combined.stats.cache_peak_capacity, part.stats.cache_peak_capacity);
        combined.stats.cache_peak_memory_bytes = std::max(
            combined.stats.cache_peak_memory_bytes, part.stats.cache_peak_memory_bytes);
        combined.stats.cache_bytes_per_state = combined.stats.cache_peak_entries == 0 ? 0.0 :
            static_cast<double>(combined.stats.cache_peak_memory_bytes) /
            static_cast<double>(combined.stats.cache_peak_entries);
        combined.stats.cache_saturated = combined.stats.cache_saturated || part.stats.cache_saturated;
        for (std::size_t d = 0; d < combined.stats.node_memo_hits_by_depth.size(); ++d)
            combined.stats.node_memo_hits_by_depth[d] += part.stats.node_memo_hits_by_depth[d];
        combined.stats.node_memo_computations += part.stats.node_memo_computations;
        combined.stats.node_memo_prunes += part.stats.node_memo_prunes;
        combined.stats.node_memo_child_rejections += part.stats.node_memo_child_rejections;
        combined.stats.node_memo_collisions += part.stats.node_memo_collisions;
        combined.stats.node_memo_saturation += part.stats.node_memo_saturation;
        combined.stats.node_memo_memory_bytes = std::max(
            combined.stats.node_memo_memory_bytes, part.stats.node_memo_memory_bytes);
        combined.stats.node_memo_available = combined.stats.node_memo_available ||
            part.stats.node_memo_available;
        combined.stats.node_state_updates += part.stats.node_state_updates;
        combined.stats.residual_histogram_updates += part.stats.residual_histogram_updates;
        combined.stats.node_sorts_avoided += part.stats.node_sorts_avoided;
        combined.stats.best_next_bucket_checks += part.stats.best_next_bucket_checks;
        combined.stats.best_next_bucket_parent_prunes += part.stats.best_next_bucket_parent_prunes;
        combined.stats.best_next_bucket_candidates_avoided += part.stats.best_next_bucket_candidates_avoided;
        combined.stats.candidate_scan_checks += part.stats.candidate_scan_checks;
        combined.stats.candidate_index_gathers += part.stats.candidate_index_gathers;
        combined.stats.candidate_index_bucket_slots_visited +=
            part.stats.candidate_index_bucket_slots_visited;
        combined.stats.candidate_index_vertices_emitted +=
            part.stats.candidate_index_vertices_emitted;
        combined.stats.candidate_index_forward_updates +=
            part.stats.candidate_index_forward_updates;
        combined.stats.candidate_index_rollback_updates +=
            part.stats.candidate_index_rollback_updates;
        combined.stats.candidate_index_cross_checks +=
            part.stats.candidate_index_cross_checks;
        combined.stats.local_continuation_calls +=
            part.stats.local_continuation_calls;
        combined.stats.local_continuation_slack_gate_skips +=
            part.stats.local_continuation_slack_gate_skips;
        combined.stats.local_continuation_branch_gate_skips +=
            part.stats.local_continuation_branch_gate_skips;
        combined.stats.local_continuation_inconclusive +=
            part.stats.local_continuation_inconclusive;
        combined.stats.local_continuation_states +=
            part.stats.local_continuation_states;
        combined.stats.local_continuation_parent_prunes +=
            part.stats.local_continuation_parent_prunes;
        combined.stats.local_continuation_nanoseconds +=
            part.stats.local_continuation_nanoseconds;
        combined.stats.local_continuation_cross_checks +=
            part.stats.local_continuation_cross_checks;
        combined.stats.partial_bounds.residual_degree_session_ceiling_skips +=
            part.stats.partial_bounds.residual_degree_session_ceiling_skips;
        combined.stats.partial_bounds.degeneracy_session_ceiling_skips +=
            part.stats.partial_bounds.degeneracy_session_ceiling_skips;
        combined.stats.partial_bounds.expensive_slack_gate_skips +=
            part.stats.partial_bounds.expensive_slack_gate_skips;
        combined.stats.partial_bounds.evaluations +=
            part.stats.partial_bounds.evaluations;
        combined.stats.partial_bounds.residual_degree_evaluations +=
            part.stats.partial_bounds.residual_degree_evaluations;
        combined.stats.partial_bounds.edge_distance_area_evaluations +=
            part.stats.partial_bounds.edge_distance_area_evaluations;
        combined.stats.partial_bounds.degree_distance_area_evaluations +=
            part.stats.partial_bounds.degree_distance_area_evaluations;
        combined.stats.partial_bounds.degeneracy_evaluations +=
            part.stats.partial_bounds.degeneracy_evaluations;
        combined.stats.partial_bounds.residual_degree_prunes +=
            part.stats.partial_bounds.residual_degree_prunes;
        combined.stats.partial_bounds.edge_distance_area_prunes +=
            part.stats.partial_bounds.edge_distance_area_prunes;
        combined.stats.partial_bounds.degree_distance_area_prunes +=
            part.stats.partial_bounds.degree_distance_area_prunes;
        combined.stats.partial_bounds.degeneracy_prunes +=
            part.stats.partial_bounds.degeneracy_prunes;
        combined.stats.partial_bounds.lagrangian_evaluations +=
            part.stats.partial_bounds.lagrangian_evaluations;
        combined.stats.partial_bounds.lagrangian_mincuts +=
            part.stats.partial_bounds.lagrangian_mincuts;
        combined.stats.partial_bounds.lagrangian_certified_prunes +=
            part.stats.partial_bounds.lagrangian_certified_prunes;
        combined.stats.partial_bounds.lagrangian_slack_gate_skips +=
            part.stats.partial_bounds.lagrangian_slack_gate_skips;
        combined.stats.partial_bounds.lagrangian_residual_gate_skips +=
            part.stats.partial_bounds.lagrangian_residual_gate_skips;
        combined.stats.partial_bounds.lagrangian_ineligible_gate_skips +=
            part.stats.partial_bounds.lagrangian_ineligible_gate_skips;
        combined.stats.partial_bounds.lagrangian_overflow_gate_skips +=
            part.stats.partial_bounds.lagrangian_overflow_gate_skips;
        combined.stats.heuristic_interval_evaluations +=
            part.stats.heuristic_interval_evaluations;
        combined.stats.heuristic_full_fallbacks += part.stats.heuristic_full_fallbacks;
        combined.stats.heuristic_runtime_seconds += part.stats.heuristic_runtime_seconds;
        combined.stats.time_to_final_upper_bound_seconds = std::max(
            combined.stats.time_to_final_upper_bound_seconds,
            part.stats.time_to_final_upper_bound_seconds);
        combined.stats.heuristic_spectral_seeds += part.stats.heuristic_spectral_seeds;
        combined.stats.heuristic_grasp_constructions += part.stats.heuristic_grasp_constructions;
        combined.stats.heuristic_vns_evaluations += part.stats.heuristic_vns_evaluations;
        combined.stats.heuristic_portfolio_improvements +=
            part.stats.heuristic_portfolio_improvements;
        combined.stats.sdp_state_requests += part.stats.sdp_state_requests;
        combined.stats.sdp_state_certified += part.stats.sdp_state_certified;
        combined.stats.sdp_state_prunes += part.stats.sdp_state_prunes;
        combined.stats.sdp_state_cache_hits += part.stats.sdp_state_cache_hits;
        combined.stats.sdp_state_calls += part.stats.sdp_state_calls;
        combined.stats.sdp_state_busy += part.stats.sdp_state_busy;
        combined.stats.sdp_state_budget_rejections += part.stats.sdp_state_budget_rejections;
        combined.stats.sdp_state_uncertified += part.stats.sdp_state_uncertified;
        combined.stats.sdp_state_dimension_rejections += part.stats.sdp_state_dimension_rejections;
        combined.stats.sdp_state_preferred_max_dimension = part.stats.sdp_state_preferred_max_dimension;
        combined.stats.pb_calls += part.stats.pb_calls;
        combined.stats.pb_sat_certificates += part.stats.pb_sat_certificates;
        combined.stats.pb_unsat_certificates += part.stats.pb_unsat_certificates;
        if (part.stats.pb_calls != 0) combined.stats.pb_last = part.stats.pb_last;
        combined.stats.pb_incremental_attempted =
            combined.stats.pb_incremental_attempted || part.stats.pb_incremental_attempted;
        combined.stats.pb_incremental_available =
            combined.stats.pb_incremental_available || part.stats.pb_incremental_available;
        combined.stats.pb_incremental_calls += part.stats.pb_incremental_calls;
        combined.stats.pb_incremental_sat += part.stats.pb_incremental_sat;
        combined.stats.pb_incremental_unsat_exploratory +=
            part.stats.pb_incremental_unsat_exploratory;
        combined.stats.pb_incremental_seconds += part.stats.pb_incremental_seconds;
        combined.stats.partial_bounds.evaluations += part.stats.partial_bounds.evaluations;
        combined.stats.partial_bounds.residual_degree_prunes += part.stats.partial_bounds.residual_degree_prunes;
        combined.stats.partial_bounds.edge_distance_area_prunes += part.stats.partial_bounds.edge_distance_area_prunes;
        combined.stats.partial_bounds.degree_distance_area_prunes += part.stats.partial_bounds.degree_distance_area_prunes;
        combined.stats.partial_bounds.degeneracy_prunes += part.stats.partial_bounds.degeneracy_prunes;
        combined.stats.residual_dp_service_calls += part.stats.residual_dp_service_calls;
        combined.stats.residual_dp_states += part.stats.residual_dp_states;
        combined.stats.residual_dp_projected_bytes = std::max(
            combined.stats.residual_dp_projected_bytes, part.stats.residual_dp_projected_bytes);
        combined.stats.residual_dp_applicable = combined.stats.residual_dp_applicable ||
            part.stats.residual_dp_applicable;
        combined.stats.residual_dp_admitted = combined.stats.residual_dp_admitted ||
            part.stats.residual_dp_admitted;
        if (combined.stats.residual_dp_skip_reason.empty())
            combined.stats.residual_dp_skip_reason = part.stats.residual_dp_skip_reason;
        combined.stats.residual_dp_completed = combined.stats.residual_dp_completed ||
            part.stats.residual_dp_completed;
        combined.stats.residual_dp_attempts += part.stats.residual_dp_attempts;
        combined.stats.residual_dp_admissions += part.stats.residual_dp_admissions;
        combined.stats.residual_dp_governor_or_cap_rejections += part.stats.residual_dp_governor_or_cap_rejections;
        combined.stats.residual_dp_completed_tails += part.stats.residual_dp_completed_tails;
        combined.stats.residual_dp_infeasible_prunes += part.stats.residual_dp_infeasible_prunes;
        combined.stats.residual_dp_feasible_witnesses += part.stats.residual_dp_feasible_witnesses;
        combined.stats.residual_dp_peak_bytes = std::max(
            combined.stats.residual_dp_peak_bytes, part.stats.residual_dp_peak_bytes);
        combined.stats.residual_dp_seconds += part.stats.residual_dp_seconds;
        combined.stats.residual_dp_cold_restarts += part.stats.residual_dp_cold_restarts;
        combined.stats.pb_sat_root_attempts += part.stats.pb_sat_root_attempts;
        combined.stats.pb_sat_root_sat += part.stats.pb_sat_root_sat;
        combined.stats.pb_sat_root_certified_unsat += part.stats.pb_sat_root_certified_unsat;
        combined.stats.pb_sat_root_timeouts += part.stats.pb_sat_root_timeouts;
        combined.stats.pb_sat_root_failures += part.stats.pb_sat_root_failures;
        combined.stats.pb_sat_root_checker_successes += part.stats.pb_sat_root_checker_successes;
        combined.stats.pb_sat_root_solver_seconds += part.stats.pb_sat_root_solver_seconds;
        combined.stats.pb_sat_root_checker_seconds += part.stats.pb_sat_root_checker_seconds;
        if (!part.stats.pb_sat_root_last_cnf_path.empty()) {
            combined.stats.pb_sat_root_active_threshold = part.stats.pb_sat_root_active_threshold;
            combined.stats.pb_sat_root_active_cardinality = part.stats.pb_sat_root_active_cardinality;
            combined.stats.pb_sat_root_last_cnf_path = part.stats.pb_sat_root_last_cnf_path;
            combined.stats.pb_sat_root_last_proof_path = part.stats.pb_sat_root_last_proof_path;
            combined.stats.pb_sat_root_last_result = part.stats.pb_sat_root_last_result;
        }
        combined.stats.components_solved += part.optimal ? 1 : 0;
        for (const auto local : part.ordering)
            combined.ordering.push_back(component.parent_vertices[local]);
        if (!part.optimal && options.time_limit.count() != 0 &&
            Clock::now() - start >= options.time_limit) break;
    }
    // If time expired before all components were processed, append their
    // vertices in component order to retain a complete feasible ordering.
    std::vector<bool> used(graph.size(), false);
    for (const auto v : combined.ordering) used[v] = true;
    for (Vertex v = 0; v < graph.size(); ++v) if (!used[v]) combined.ordering.push_back(v);
    combined.upper_bound = graph.ordering_cutwidth(combined.ordering);
    if (root_seed && root_seed->first < combined.upper_bound) {
        combined.upper_bound = root_seed->first;
        combined.ordering = std::move(root_seed->second);
    }
    if (milp && milp->incumbent_width && graph.validate_ordering(milp->ordering) &&
        *milp->incumbent_width < combined.upper_bound) {
        combined.ordering = std::move(milp->ordering);
        combined.upper_bound = *milp->incumbent_width;
        combined.stats.milp_incumbent_accepted = true;
    }
    if (sdp_root_bound) combined.lower_bound = std::max(combined.lower_bound, *sdp_root_bound);
    combined.optimal = combined.lower_bound == combined.upper_bound;
    if (combined.optimal) combined.lower_bound = combined.upper_bound;
    return combined;
}

} // namespace cutwidth
