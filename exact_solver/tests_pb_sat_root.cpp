#include "graph.hpp"
#include "optimizer_v2.hpp"

#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <thread>
#include <stdexcept>

#ifndef FAKE_TOOL_PATH
#define FAKE_TOOL_PATH "./cutwidth_fake_tool"
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

using namespace cutwidth;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

struct TestFixture {
    std::filesystem::path temp_dir;
    std::string solver_path;
    std::string checker_path;

    TestFixture() {
        temp_dir = std::filesystem::temp_directory_path() / "cutwidth_tests_pb_sat_root_temp";
        std::filesystem::create_directories(temp_dir);
#ifdef _WIN32
        solver_path = (temp_dir / "fake_solver.exe").string();
        checker_path = (temp_dir / "fake_checker.exe").string();
#else
        solver_path = (temp_dir / "fake_solver").string();
        checker_path = (temp_dir / "fake_checker").string();
#endif
        std::error_code ec;
        std::filesystem::copy_file(FAKE_TOOL_PATH, solver_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "Failed to copy solver: " << ec.message() << "\n";
        }
        std::filesystem::copy_file(FAKE_TOOL_PATH, checker_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "Failed to copy checker: " << ec.message() << "\n";
        }

#if defined(__unix__) || defined(__APPLE__)
        ::chmod(solver_path.c_str(), 0755);
        ::chmod(checker_path.c_str(), 0755);
#endif
    }

    ~TestFixture() {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }
};

Graph test_graph() {
    constexpr Graph::Vertex side = 15;
    std::vector<std::pair<Graph::Vertex, Graph::Vertex>> edges;
    for (Graph::Vertex row = 0; row < side; ++row) {
        for (Graph::Vertex column = 0; column < side; ++column) {
            const auto vertex = row * side + column;
            if (row + 1 < side) edges.emplace_back(vertex, vertex + side);
            if (column + 1 < side) edges.emplace_back(vertex, vertex + 1);
        }
    }
    return Graph(side * side, edges);
}

void configure_test_budget(OptimizerV2Options& options) {
    options.time_limit = std::chrono::milliseconds(1000);
    options.pb_sat_root_max_gap = 32;
    // These tests deliberately exercise the process-backed compatibility
    // backend with fake solver/checker executables.
    options.pb_sat_root_backend = PbSatRootBackend::external;
}

void test_disabled() {
    std::cout << "Running test_disabled...\n";
    TestFixture fixture;

    OptimizerV2Options options;
    options.controller = ControllerMode::adaptive;
    options.adaptive_arms = {"dfs"}; // pb-sat-root is NOT enabled
    options.pb_sat_root_solver = fixture.solver_path;
    options.pb_sat_root_checker = fixture.checker_path;
    options.pb_sat_root_dir = fixture.temp_dir.string();
    options.pb_sat_root_timeout = std::chrono::milliseconds(500);
    configure_test_budget(options);

    set_env_var("FAKE_SOLVER_BEHAVIOR", "solver_unsat");
    set_env_var("FAKE_CHECKER_BEHAVIOR", "checker_success");

    auto result = optimize_cutwidth_v2(test_graph(), options);

    require(result.stats.pb_sat_root_attempts == 0, "pb-sat-root should not run when disabled");
    std::cout << "test_disabled passed.\n";
}

void test_certified_unsat() {
    std::cout << "Running test_certified_unsat...\n";
    TestFixture fixture;

    OptimizerV2Options options;
    options.controller = ControllerMode::adaptive;
    options.adaptive_arms = {"dfs", "pb-sat-root"};
    options.pb_sat_root_solver = fixture.solver_path;
    options.pb_sat_root_checker = fixture.checker_path;
    options.pb_sat_root_dir = fixture.temp_dir.string();
    options.pb_sat_root_timeout = std::chrono::milliseconds(500);
    configure_test_budget(options);

    set_env_var("FAKE_SOLVER_BEHAVIOR", "solver_unsat");
    set_env_var("FAKE_CHECKER_BEHAVIOR", "checker_success");

    auto result = optimize_cutwidth_v2(test_graph(), options);

    require(result.stats.pb_sat_root_attempts > 0, "pb-sat-root should have been attempted");
    require(result.stats.pb_sat_root_certified_unsat > 0, "should have certified UNSAT");
    require(result.stats.pb_sat_root_checker_successes > 0, "checker should have succeeded");
    require(result.stats.pb_sat_root_last_result == "CERTIFIED_UNSAT", "last result should be CERTIFIED_UNSAT");
    std::cout << "test_certified_unsat passed.\n";
}

void test_unchecked_unsat() {
    std::cout << "Running test_unchecked_unsat...\n";
    TestFixture fixture;

    OptimizerV2Options options;
    options.controller = ControllerMode::adaptive;
    options.adaptive_arms = {"dfs", "pb-sat-root"};
    options.pb_sat_root_solver = fixture.solver_path;
    options.pb_sat_root_checker = fixture.checker_path;
    options.pb_sat_root_dir = fixture.temp_dir.string();
    options.pb_sat_root_timeout = std::chrono::milliseconds(500);
    configure_test_budget(options);

    set_env_var("FAKE_SOLVER_BEHAVIOR", "solver_unsat");
    set_env_var("FAKE_CHECKER_BEHAVIOR", "checker_fail"); // checker fails!

    auto result = optimize_cutwidth_v2(test_graph(), options);

    require(result.stats.pb_sat_root_attempts > 0, "pb-sat-root should have been attempted");
    require(result.stats.pb_sat_root_certified_unsat == 0, "should NOT have certified UNSAT because checker failed");
    require(result.stats.pb_sat_root_failures > 0, "should record a failure");
    require(result.stats.pb_sat_root_last_result == "FAILURE", "last result should be FAILURE");
    std::cout << "test_unchecked_unsat passed.\n";
}

void test_sat() {
    std::cout << "Running test_sat...\n";
    TestFixture fixture;

    OptimizerV2Options options;
    options.controller = ControllerMode::adaptive;
    options.adaptive_arms = {"dfs", "pb-sat-root"};
    options.pb_sat_root_solver = fixture.solver_path;
    options.pb_sat_root_checker = fixture.checker_path;
    options.pb_sat_root_dir = fixture.temp_dir.string();
    options.pb_sat_root_timeout = std::chrono::milliseconds(500);
    configure_test_budget(options);

    set_env_var("FAKE_SOLVER_BEHAVIOR", "solver_sat");

    auto result = optimize_cutwidth_v2(test_graph(), options);

    require(result.stats.pb_sat_root_attempts > 0, "pb-sat-root should have been attempted");
    require(result.stats.pb_sat_root_sat > 0, "should record SAT");
    require(result.stats.pb_sat_root_certified_unsat == 0, "should NOT have certified UNSAT");
    require(result.stats.pb_sat_root_last_result == "SAT", "last result should be SAT");
    std::cout << "test_sat passed.\n";
}

void test_timeout() {
    std::cout << "Running test_timeout...\n";
    TestFixture fixture;

    OptimizerV2Options options;
    options.controller = ControllerMode::adaptive;
    options.adaptive_arms = {"dfs", "pb-sat-root"};
    options.pb_sat_root_solver = fixture.solver_path;
    options.pb_sat_root_checker = fixture.checker_path;
    options.pb_sat_root_dir = fixture.temp_dir.string();
    options.pb_sat_root_timeout = std::chrono::milliseconds(10); // very small timeout!
    configure_test_budget(options);

    set_env_var("FAKE_SOLVER_BEHAVIOR", "solver_timeout");

    auto result = optimize_cutwidth_v2(test_graph(), options);

    require(result.stats.pb_sat_root_attempts > 0, "pb-sat-root should have been attempted");
    require(result.stats.pb_sat_root_timeouts > 0 || result.stats.pb_sat_root_failures > 0, "should record a timeout or failure");
    require(result.stats.pb_sat_root_certified_unsat == 0, "should NOT have certified UNSAT");
    std::cout << "test_timeout passed.\n";
}

int main() {
    try {
        test_disabled();
        test_certified_unsat();
        test_unchecked_unsat();
        test_sat();
        test_timeout();
        std::cout << "All pb-sat-root tests passed successfully!\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test suite failed: " << error.what() << "\n";
        return 1;
    }
}
