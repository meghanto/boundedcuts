#include "pb_solver.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iomanip>
#include <sstream>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cutwidth::pb {
namespace {

using Clock = std::chrono::steady_clock;

struct TempDirectory {
    std::filesystem::path path;
    bool keep = false;
    ~TempDirectory() {
        if (!keep && !path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

bool write_dimacs(const CnfFormula& formula,
                  const std::vector<std::int32_t>& unit_clauses,
                  const std::filesystem::path& path,
                  std::string& error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { error = "could not create DIMACS input"; return false; }
    output << "p cnf " << formula.variable_count << ' '
           << formula.clauses.size() + unit_clauses.size() << '\n';
    for (const auto& clause : formula.clauses) {
        for (const auto literal : clause) {
            if (literal == 0 || literal == std::numeric_limits<std::int32_t>::min() ||
                static_cast<std::uint64_t>(literal < 0 ? -static_cast<std::int64_t>(literal)
                                                       : literal) > formula.variable_count) {
                error = "DIMACS clause contains an out-of-range literal";
                return false;
            }
            output << literal << ' ';
        }
        output << "0\n";
    }
    for (const auto literal : unit_clauses) {
        if (literal == 0 || literal == std::numeric_limits<std::int32_t>::min() ||
            static_cast<std::uint64_t>(literal < 0 ? -static_cast<std::int64_t>(literal)
                                                   : literal) > formula.variable_count) {
            error = "DIMACS unit clause contains an out-of-range literal";
            return false;
        }
        output << literal << " 0\n";
    }
    output.flush();
    if (!output) { error = "failed while writing DIMACS input"; return false; }
    return true;
}

std::vector<std::string> expand_arguments(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& input, const std::filesystem::path& proof) {
    std::vector<std::string> result;
    result.reserve(arguments.size());
    for (auto argument : arguments) {
        auto replace = [&argument](std::string_view token, const std::string& value) {
            std::size_t position = 0;
            while ((position = argument.find(token, position)) != std::string::npos) {
                argument.replace(position, token.size(), value);
                position += value.size();
            }
        };
        replace("{input}", input.string());
        replace("{proof}", proof.string());
        result.push_back(std::move(argument));
    }
    return result;
}

struct ProcessResult {
    bool launched = false;
    bool timed_out = false;
    int exit_code = -1;
};

#if defined(__unix__) || defined(__APPLE__)
ProcessResult run_process(const std::string& executable,
                          const std::vector<std::string>& arguments,
                          const std::filesystem::path& output_path,
                          Clock::time_point deadline) {
    ProcessResult result;
    const int output = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0) return result;
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(output);
        return result;
    }
    if (::posix_spawnattr_init(&attributes) != 0) {
        (void)::posix_spawn_file_actions_destroy(&actions);
        ::close(output);
        return result;
    }
    (void)::posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    (void)::posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
    (void)::posix_spawn_file_actions_addclose(&actions, output);
    (void)::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    (void)::posix_spawnattr_setpgroup(&attributes, 0);
    pid_t child = -1;
    const int spawn_error = ::posix_spawn(
        &child, executable.c_str(), &actions, &attributes, argv.data(), environ);
    (void)::posix_spawn_file_actions_destroy(&actions);
    (void)::posix_spawnattr_destroy(&attributes);
    ::close(output);
    if (spawn_error != 0 || child < 0) return result;
    result.launched = true;
    int status = 0;
    while (true) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) return result;
        if (Clock::now() >= deadline) {
            result.timed_out = true;
            // The child is its own process group, so helper processes are also
            // terminated. Fall back to the direct PID if setpgid raced.
            (void)::kill(-child, SIGTERM);
            (void)::kill(child, SIGTERM);
            for (unsigned attempt = 0; attempt != 20; ++attempt) {
                if (::waitpid(child, &status, WNOHANG) == child) goto reaped;
                ::usleep(10000);
            }
            (void)::kill(-child, SIGKILL);
            (void)::kill(child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        ::usleep(10000);
    }
reaped:
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    return result;
}
#endif

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

bool parse_model(const std::string& output, std::size_t variables,
                 std::vector<std::int8_t>& assignment) {
    assignment.assign(variables + 1, -1);
    std::istringstream lines(output);
    std::string line;
    bool saw_literal = false;
    while (std::getline(lines, line)) {
        if (line.empty() || (line[0] != 'v' && line[0] != 'V')) continue;
        std::istringstream values(line.substr(1));
        std::int64_t literal = 0;
        while (values >> literal) {
            if (literal == 0) continue;
            const auto variable = static_cast<std::uint64_t>(literal < 0 ? -literal : literal);
            if (variable == 0 || variable > variables) return false;
            const std::int8_t value = literal > 0 ? 1 : 0;
            if (assignment[variable] != -1 && assignment[variable] != value) return false;
            assignment[variable] = value;
            saw_literal = true;
        }
    }
    return saw_literal || variables == 0;
}

bool executable_path_valid(const std::string& path) {
    if (path.empty() || !std::filesystem::path(path).is_absolute()) return false;
#if defined(__unix__) || defined(__APPLE__)
    return ::access(path.c_str(), X_OK) == 0;
#else
    return false;
#endif
}

std::string hash_file_fnv1a64_impl(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::uint64_t hash = 14695981039346656037ULL;
    char buffer[1U << 16U];
    while (input) {
        input.read(buffer, sizeof buffer);
        const auto count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    if (input.bad()) return {};
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

} // namespace

ExternalSatResult solve_dimacs_external(
    const CnfFormula& formula, const ExternalSatOptions& options) {
    ExternalSatResult result;
#if !defined(__unix__) && !defined(__APPLE__)
    (void)formula; (void)options;
    result.diagnostic = "external SAT process adapter is unavailable on this platform";
    return result;
#else
    if (!executable_path_valid(options.solver_path)) {
        result.diagnostic = "solver path must name an executable absolute path";
        return result;
    }
    const auto started = Clock::now();
    const auto deadline = options.time_limit.count() == 0
        ? Clock::time_point::max() : started + options.time_limit;
    char directory_template[] = "/tmp/cutwidth-pb-XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        result.status = ExternalSatStatus::process_error;
        result.diagnostic = "could not create private temporary directory";
        return result;
    }
    TempDirectory temporary{directory, options.keep_temporary_files};
    result.temporary_directory = temporary.path.string();
    const auto input_path = temporary.path / "instance.cnf";
    const auto proof_path = temporary.path / "proof.out";
    const auto solver_output_path = temporary.path / "solver.log";
    const auto checker_output_path = temporary.path / "checker.log";
    const auto version_output_path = temporary.path / "version.log";
    if (!write_dimacs(formula, options.unit_clauses, input_path, result.diagnostic)) {
        result.status = ExternalSatStatus::process_error;
        return result;
    }

    const auto version = run_process(options.solver_path, options.version_arguments,
                                     version_output_path, deadline);
    result.solver_version = read_file(version_output_path);
    while (!result.solver_version.empty() &&
           (result.solver_version.back() == '\n' || result.solver_version.back() == '\r'))
        result.solver_version.pop_back();
    if (version.timed_out) { result.status = ExternalSatStatus::timed_out; return result; }
    if (!version.launched || version.exit_code != 0) {
        result.status = ExternalSatStatus::unavailable;
        result.diagnostic = "solver version probe failed";
        return result;
    }
    if (!options.expected_version.empty() &&
        result.solver_version.find(options.expected_version) == std::string::npos) {
        result.status = ExternalSatStatus::version_mismatch;
        result.diagnostic = "solver version does not match configured pin";
        return result;
    }

    auto solver_deadline = deadline;
    if (options.time_limit.count() != 0 && options.proof_check_reserve.count() > 0) {
        const auto reserve = std::min(options.proof_check_reserve, options.time_limit);
        solver_deadline -= reserve;
        if (Clock::now() >= solver_deadline) {
            result.status = ExternalSatStatus::timed_out;
            result.diagnostic = "proof-check reserve exhausted the SAT solving budget";
            return result;
        }
    }
    const auto arguments = expand_arguments(options.solver_arguments, input_path, proof_path);
    const auto solve_started = Clock::now();
    const auto process = run_process(
        options.solver_path, arguments, solver_output_path, solver_deadline);
    result.runtime_seconds = std::chrono::duration<double>(Clock::now() - solve_started).count();
    result.solver_exit_code = process.exit_code;
    result.solver_output = read_file(solver_output_path);
    if (process.timed_out) { result.status = ExternalSatStatus::timed_out; return result; }
    if (!process.launched || process.exit_code == 127) {
        result.status = ExternalSatStatus::process_error;
        result.diagnostic = "solver process could not be executed";
        return result;
    }
    const auto parsed = parse_status(result.solver_output, process.exit_code);
    if (parsed == ParsedStatus::sat) {
        if (!parse_model(result.solver_output, formula.variable_count, result.assignment)) {
            result.status = ExternalSatStatus::invalid_output;
            result.diagnostic = "SAT result did not contain a consistent DIMACS model";
            return result;
        }
        result.status = ExternalSatStatus::sat;
        return result;
    }
    if (parsed != ParsedStatus::unsat) {
        result.status = ExternalSatStatus::invalid_output;
        result.diagnostic = "solver output contained neither SAT nor UNSAT";
        return result;
    }
    std::error_code proof_error;
    result.proof_generated = std::filesystem::is_regular_file(proof_path, proof_error) &&
        !proof_error && std::filesystem::file_size(proof_path, proof_error) != 0 && !proof_error;
    if (result.proof_generated) {
        result.proof_bytes = std::filesystem::file_size(proof_path, proof_error);
        if (!proof_error) result.proof_fnv1a64 = hash_file_fnv1a64_impl(proof_path);
    }
    if (!executable_path_valid(options.proof_checker_path) ||
        !result.proof_generated) {
        result.status = ExternalSatStatus::unsat_unverified;
        result.diagnostic = "UNSAT rejected because no proof checker/proof was available";
        return result;
    }
    const auto checker_arguments = expand_arguments(
        options.proof_checker_arguments, input_path, proof_path);
    const auto checker_started = Clock::now();
    const auto checker = run_process(options.proof_checker_path, checker_arguments,
                                     checker_output_path, deadline);
    result.checker_seconds = std::chrono::duration<double>(Clock::now() - checker_started).count();
    result.checker_exit_code = checker.exit_code;
    result.checker_output = read_file(checker_output_path);
    if (checker.timed_out) { result.status = ExternalSatStatus::timed_out; return result; }
    if (!checker.launched || checker.exit_code != 0) {
        result.status = ExternalSatStatus::unsat_unverified;
        result.diagnostic = "UNSAT proof checker did not exit successfully";
        return result;
    }
    result.proof_checked = true;
    result.status = ExternalSatStatus::unsat_verified;
    return result;
#endif
}

ProofCheckResult check_drat_proof_external(
    const CnfFormula& formula, const std::vector<std::int32_t>& unit_clauses,
    const std::string& proof_path, const ExternalSatOptions& options) {
    ProofCheckResult result;
#if !defined(__unix__) && !defined(__APPLE__)
    (void)formula; (void)unit_clauses; (void)proof_path; (void)options;
    result.diagnostic = "external proof checker is unavailable on this platform";
    return result;
#else
    if (!executable_path_valid(options.proof_checker_path) ||
        !std::filesystem::is_regular_file(proof_path)) {
        result.diagnostic = "proof checker or proof file is unavailable";
        return result;
    }
    const auto started = Clock::now();
    const auto deadline = options.time_limit.count() == 0
        ? Clock::time_point::max() : started + options.time_limit;
    char directory_template[] = "/tmp/cutwidth-pb-check-XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    if (directory == nullptr) {
        result.diagnostic = "could not create proof-check directory";
        return result;
    }
    TempDirectory temporary{directory, false};
    const auto input_path = temporary.path / "instance.cnf";
    const auto output_path = temporary.path / "checker.log";
    if (!write_dimacs(formula, unit_clauses, input_path, result.diagnostic)) return result;
    const auto arguments = expand_arguments(
        options.proof_checker_arguments, input_path, proof_path);
    const auto process = run_process(
        options.proof_checker_path, arguments, output_path, deadline);
    result.runtime_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    result.exit_code = process.exit_code;
    result.output = read_file(output_path);
    result.timed_out = process.timed_out;
    result.checked = process.launched && !process.timed_out && process.exit_code == 0;
    if (!result.checked)
        result.diagnostic = process.timed_out ? "proof checker timed out" :
            "proof checker did not exit successfully";
    return result;
#endif
}

std::string file_fnv1a64(const std::string& path) {
    return hash_file_fnv1a64_impl(path);
}

} // namespace cutwidth::pb
