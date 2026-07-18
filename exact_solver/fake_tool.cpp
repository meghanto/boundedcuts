#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <algorithm>
#include <cctype>

int main(int argc, char** argv) {
    if (argc < 1) return 1;
    std::string arg0 = argv[0];
    for (auto& c : arg0) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    bool is_solver = arg0.find("solver") != std::string::npos;
    bool is_checker = arg0.find("checker") != std::string::npos;

    std::string behavior;
    if (is_solver) {
        const char* val = std::getenv("FAKE_SOLVER_BEHAVIOR");
        behavior = val ? val : "";
    } else if (is_checker) {
        const char* val = std::getenv("FAKE_CHECKER_BEHAVIOR");
        behavior = val ? val : "";
    } else {
        const char* val = std::getenv("FAKE_TOOL_BEHAVIOR");
        behavior = val ? val : "";
    }

    if (behavior == "solver_unsat") {
        if (argc < 3) {
            std::cerr << "fake_solver error: solver_unsat needs input and proof paths\n";
            return 1;
        }
        std::cout << "s UNSATISFIABLE\n";
        std::ofstream proof(argv[argc - 1]);
        proof << "dummy proof\n";
        return 20;
    } else if (behavior == "solver_sat") {
        std::cout << "s SATISFIABLE\n";
        return 10;
    } else if (behavior == "solver_timeout" || behavior == "checker_timeout") {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 0;
    } else if (behavior == "solver_fail" || behavior == "checker_fail") {
        return 1;
    } else if (behavior == "checker_success") {
        return 0;
    }

    std::cerr << "Unknown fake tool behavior: " << behavior
              << " (is_solver=" << is_solver << ", is_checker=" << is_checker << ")\n";
    return 99;
}
