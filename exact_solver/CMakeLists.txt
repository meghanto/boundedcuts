cmake_minimum_required(VERSION 3.20)

project(cutwidth_exact VERSION 0.1.0 LANGUAGES C CXX)

option(CUTWIDTH_BUILD_TESTS "Build the cutwidth test suite" ON)
option(CUTWIDTH_ENABLE_SDP_PROTOTYPE "Build experimental SDP operator prototype" OFF)
option(CUTWIDTH_ENABLE_ONETBB "Build the legacy oneTBB parallel executor compatibility path" OFF)
set(CUTWIDTH_CLARABEL_ROOT "" CACHE PATH "Pinned Clarabel.cpp source root (optional)")
set(CUTWIDTH_CADICAL_ROOT "" CACHE PATH "Pinned CaDiCaL 2.1.3 source root (optional)")
set(CUTWIDTH_DRAT_TRIM_ROOT "" CACHE PATH "Pinned DRAT-trim source root (optional)")

if(CUTWIDTH_CADICAL_ROOT OR CUTWIDTH_DRAT_TRIM_ROOT)
    find_package(Git REQUIRED)
endif()
if(CUTWIDTH_DRAT_TRIM_ROOT)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CUTWIDTH_DRAT_TRIM_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE DRAT_TRIM_PIN OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT DRAT_TRIM_PIN STREQUAL "2e3b2dc0ecf938addbd779d42877b6ed69d9a985")
        message(FATAL_ERROR "CUTWIDTH_DRAT_TRIM_ROOT is not the pinned DRAT-trim revision")
    endif()
endif()

add_library(cutwidth_solver
    canonical_ownership.cpp
    checkpoint.cpp
    checkpoint_v2.cpp
    decision.cpp
    decision_session.cpp
    decision_cache.cpp
    decomposition.cpp
    global_dfs_executor.cpp
    graph.cpp
    incumbent_session.cpp
    incremental_layout.cpp
    memory_governor.cpp
    milp_adapter.cpp
    node_memo.cpp
    optimizer_v2.cpp
    parallel_decision_session.cpp
    parallel_global_dfs_session.cpp
    partial_bounds.cpp
    pb_backend.cpp
    pb_cadical_incremental.cpp
    pb_encoding.cpp
    pb_solver.cpp
    residual_dp.cpp
    progressive_sdp_session.cpp
    progressive_cheap_bound_session.cpp
    sdp_bound_oracle.cpp
    sdp_certificate.cpp
    sdp_operator.cpp
    solver.cpp
    threshold_portfolio.cpp
    vertex_set.cpp
    lagrangian_bound.cpp
    pb_drat_trim_adapter.cpp
    drat_trim_impl.c
)
if(CUTWIDTH_CADICAL_ROOT)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CUTWIDTH_CADICAL_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE CADICAL_PIN OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT CADICAL_PIN STREQUAL "f13d74439a5b5c963ac5b02d05ce93a8098018b8")
        message(FATAL_ERROR "CUTWIDTH_CADICAL_ROOT is not pinned CaDiCaL v2.1.3")
    endif()
    if(NOT EXISTS "${CUTWIDTH_CADICAL_ROOT}/src/cadical.hpp")
        message(FATAL_ERROR "Pinned CaDiCaL source headers missing")
    endif()

    set(CADICAL_SOURCES
        "${CUTWIDTH_CADICAL_ROOT}/src/analyze.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/arena.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/assume.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/averages.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/backtrack.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/backward.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/bins.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/block.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/ccadical.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/checker.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/clause.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/collect.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/compact.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/condition.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/config.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/constrain.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/contract.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/cover.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/decide.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/decompose.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/deduplicate.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/drattracer.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/elim.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/ema.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/extend.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/external.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/external_propagate.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/file.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/flags.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/flip.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/format.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/frattracer.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/gates.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/idruptracer.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/instantiate.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/internal.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/ipasir.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/lidruptracer.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/limit.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/logging.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/lookahead.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/lratbuilder.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/lratchecker.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/lrattracer.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/lucky.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/message.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/minimize.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/occs.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/options.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/parse.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/phases.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/probe.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/profile.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/proof.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/propagate.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/queue.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/random.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/reap.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/reduce.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/rephase.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/report.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/resources.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/restart.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/restore.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/score.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/shrink.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/signal.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/solution.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/solver.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/stats.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/subsume.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/terminal.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/ternary.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/transred.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/util.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/var.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/veripbtracer.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/version.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/vivify.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/walk.cpp"
        "${CUTWIDTH_CADICAL_ROOT}/src/watch.cpp"
    )

    add_library(cadical STATIC ${CADICAL_SOURCES})
    set_property(TARGET cadical PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_include_directories(cadical PUBLIC "${CUTWIDTH_CADICAL_ROOT}/src")
    target_compile_definitions(cadical PRIVATE NBUILD)
    if(APPLE)
        # CaDiCaL's configure probe emits this on macOS versions whose SDK
        # does not declare closefrom(3).
        target_compile_definitions(cadical PRIVATE NCLOSEFROM)
    endif()
    if(MSVC)
        target_compile_definitions(cadical PRIVATE __WIN32 NFLEXIBLE)
        target_include_directories(cadical PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/msvc_compat")
        target_compile_options(cadical PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/msvc_compat/cadical_msvc.hpp")
        target_link_libraries(cadical PRIVATE Psapi)
    endif()

    target_link_libraries(cutwidth_solver PRIVATE cadical)
    target_include_directories(cutwidth_solver SYSTEM PRIVATE "${CUTWIDTH_CADICAL_ROOT}/src")
    target_compile_definitions(cutwidth_solver PUBLIC CUTWIDTH_HAVE_CADICAL=1)
    message(STATUS "Pinned native CaDiCaL incremental backend enabled via source compile")
endif()
if(CUTWIDTH_ENABLE_SDP_PROTOTYPE)
    target_sources(cutwidth_solver PRIVATE sdp_admm.cpp clarabel_sdp_adapter.cpp)
    # Conan supplies OpenBLAS (including LAPACK) on Windows, where CMake's
    # module-mode FindLAPACK cannot discover the package without manual
    # library hints. Keep the system LAPACK fallback for existing Unix builds.
    find_package(OpenBLAS CONFIG QUIET)
    if(TARGET OpenBLAS::OpenBLAS)
        target_link_libraries(cutwidth_solver PRIVATE OpenBLAS::OpenBLAS)
    else()
        find_package(LAPACK REQUIRED)
        target_link_libraries(cutwidth_solver PRIVATE ${LAPACK_LIBRARIES})
    endif()
    target_compile_definitions(cutwidth_solver PUBLIC CUTWIDTH_ENABLE_SDP_PROTOTYPE=1)
    if(CUTWIDTH_CLARABEL_ROOT)
        if(WIN32)
            set(CLARABEL_MANIFEST "${CUTWIDTH_CLARABEL_ROOT}/rust_wrapper/target/release/clarabel_sdp_manifest.txt")
            if(NOT EXISTS "${CLARABEL_MANIFEST}")
                message(FATAL_ERROR "Clarabel SDP manifest missing at ${CLARABEL_MANIFEST}. On Windows, run 'tools/bootstrap_clarabel_windows.ps1' to build Clarabel with SDP support.")
            endif()
            file(READ "${CLARABEL_MANIFEST}" MANIFEST_CONTENT)
            if(NOT MANIFEST_CONTENT MATCHES "SDP=true")
                message(FATAL_ERROR "Clarabel SDP manifest does not specify SDP=true.")
            endif()
            if(NOT MANIFEST_CONTENT MATCHES "CLARABEL_CPP_COMMIT=0de6259a3edfd5cc041ec42b2148599ce63e73cb")
                message(FATAL_ERROR "Clarabel SDP manifest specifies incorrect Clarabel.cpp pin.")
            endif()

            set(CLARABEL_RUNTIME
                "${CUTWIDTH_CLARABEL_ROOT}/rust_wrapper/target/release/clarabel_c.dll")
            set(CLARABEL_LIBRARY
                "${CUTWIDTH_CLARABEL_ROOT}/rust_wrapper/target/release/clarabel_c.dll.lib")
        else()
            set(CLARABEL_RUNTIME
                "${CUTWIDTH_CLARABEL_ROOT}/rust_wrapper/target/release/libclarabel_c${CMAKE_SHARED_LIBRARY_SUFFIX}")
            set(CLARABEL_LIBRARY "${CLARABEL_RUNTIME}")
        endif()
        if(EXISTS "${CUTWIDTH_CLARABEL_ROOT}/include/clarabel.hpp" AND
           EXISTS "${CLARABEL_LIBRARY}" AND EXISTS "${CLARABEL_RUNTIME}")
            find_package(Eigen3 CONFIG REQUIRED)
            find_package(Git REQUIRED)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CUTWIDTH_CLARABEL_ROOT}" rev-parse HEAD
                OUTPUT_VARIABLE CLARABEL_PIN OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT CLARABEL_PIN STREQUAL "0de6259a3edfd5cc041ec42b2148599ce63e73cb")
                message(FATAL_ERROR "CUTWIDTH_CLARABEL_ROOT is not pinned Clarabel.cpp v0.11.1")
            endif()
            target_include_directories(cutwidth_solver SYSTEM PRIVATE "${CUTWIDTH_CLARABEL_ROOT}/include")
            target_link_libraries(cutwidth_solver PRIVATE Eigen3::Eigen "${CLARABEL_LIBRARY}")
            target_compile_definitions(cutwidth_solver PUBLIC CUTWIDTH_HAVE_CLARABEL_SDP=1 FEATURE_SDP=1)
            if(WIN32)
                add_custom_command(TARGET cutwidth_solver POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CLARABEL_RUNTIME}" "$<TARGET_FILE_DIR:cutwidth_solver>")
            else()
                set_property(TARGET cutwidth_solver APPEND PROPERTY BUILD_RPATH
                    "${CUTWIDTH_CLARABEL_ROOT}/rust_wrapper/target/release")
            endif()
            message(STATUS "Pinned Clarabel.cpp SDP backend enabled")
        else()
            message(STATUS "Clarabel.cpp artifacts missing; SDP adapter unavailable")
        endif()
    endif()
endif()
target_include_directories(cutwidth_solver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(cutwidth_solver PUBLIC cxx_std_20)
find_package(Boost REQUIRED)
target_link_libraries(cutwidth_solver PUBLIC Boost::boost)
find_package(dispenso NAMES Dispenso CONFIG REQUIRED)
target_link_libraries(cutwidth_solver PUBLIC Dispenso::dispenso)
if(CUTWIDTH_ENABLE_ONETBB)
    find_package(TBB CONFIG QUIET COMPONENTS tbb)
    if(TBB_FOUND)
        target_link_libraries(cutwidth_solver PRIVATE TBB::tbb)
        target_compile_definitions(cutwidth_solver PUBLIC CUTWIDTH_HAVE_ONETBB=1)
        message(STATUS "Legacy oneTBB parallel executor enabled (not recommended)")
    else()
        message(STATUS "oneTBB not found; building native parallel executor only")
    endif()
endif()

option(CUTWIDTH_ENABLE_HIGHS "Enable optional in-process HiGHS MILP backend" ON)
if(CUTWIDTH_ENABLE_HIGHS)
    find_path(HIGHS_INCLUDE_DIR Highs.h PATH_SUFFIXES highs)
    find_library(HIGHS_LIBRARY NAMES highs)
    if(HIGHS_INCLUDE_DIR AND HIGHS_LIBRARY)
        target_include_directories(cutwidth_solver SYSTEM PRIVATE ${HIGHS_INCLUDE_DIR})
        target_link_libraries(cutwidth_solver PRIVATE ${HIGHS_LIBRARY})
        find_package(ZLIB QUIET)
        if(ZLIB_FOUND)
            target_link_libraries(cutwidth_solver PRIVATE ZLIB::ZLIB)
        endif()
        target_compile_definitions(cutwidth_solver PUBLIC CUTWIDTH_HAVE_HIGHS=1)
        message(STATUS "Optional HiGHS MILP backend enabled")
    else()
        message(STATUS "HiGHS not found; building without optional MILP backend")
    endif()
endif()

if(MSVC)
    target_compile_options(cutwidth_solver PRIVATE /W4 /permissive-)
else()
    target_compile_options(cutwidth_solver PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cutwidth_exact main.cpp)
target_link_libraries(cutwidth_exact PRIVATE cutwidth_solver)

if(CUTWIDTH_BUILD_TESTS)
    enable_testing()
    add_executable(cutwidth_tests tests.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests COMMAND cutwidth_tests)
    add_executable(cutwidth_tests_v2 tests_v2.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests_v2 PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_v2 COMMAND cutwidth_tests_v2)
    add_executable(cutwidth_tests_bounds tests_bounds.cpp)
    target_link_libraries(cutwidth_tests_bounds PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_bounds COMMAND cutwidth_tests_bounds)
    add_executable(cutwidth_tests_cache tests_cache.cpp)
    target_link_libraries(cutwidth_tests_cache PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_cache COMMAND cutwidth_tests_cache)
    add_executable(cutwidth_tests_node_memo tests_node_memo.cpp)
    target_link_libraries(cutwidth_tests_node_memo PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_node_memo COMMAND cutwidth_tests_node_memo)
    add_executable(cutwidth_tests_sdp_oracle tests_sdp_oracle.cpp)
    target_link_libraries(cutwidth_tests_sdp_oracle PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_sdp_oracle COMMAND cutwidth_tests_sdp_oracle)
    add_executable(cutwidth_tests_progressive_sdp_session tests_progressive_sdp_session.cpp)
    target_link_libraries(cutwidth_tests_progressive_sdp_session PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_progressive_sdp_session COMMAND cutwidth_tests_progressive_sdp_session)
    add_executable(cutwidth_tests_progressive_cheap_bound_session
        tests_progressive_cheap_bound_session.cpp)
    target_link_libraries(cutwidth_tests_progressive_cheap_bound_session PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_progressive_cheap_bound_session
        COMMAND cutwidth_tests_progressive_cheap_bound_session)
    add_executable(cutwidth_tests_checkpoint tests_checkpoint.cpp)
    target_link_libraries(cutwidth_tests_checkpoint PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_checkpoint COMMAND cutwidth_tests_checkpoint)
    add_executable(cutwidth_tests_incumbent tests_incumbent.cpp)
    target_link_libraries(cutwidth_tests_incumbent PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_incumbent COMMAND cutwidth_tests_incumbent)
    add_executable(cutwidth_tests_threshold_portfolio tests_threshold_portfolio.cpp)
    target_link_libraries(cutwidth_tests_threshold_portfolio PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_threshold_portfolio COMMAND cutwidth_tests_threshold_portfolio)
    add_executable(cutwidth_tests_decision_session tests_decision_session.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests_decision_session PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_decision_session COMMAND cutwidth_tests_decision_session)
    add_executable(cutwidth_tests_memory_governor tests_memory_governor.cpp)
    target_link_libraries(cutwidth_tests_memory_governor PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_memory_governor COMMAND cutwidth_tests_memory_governor)
    add_executable(cutwidth_tests_residual_dp tests_residual_dp.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests_residual_dp PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_residual_dp COMMAND cutwidth_tests_residual_dp)
    add_executable(cutwidth_tests_parallel_session tests_parallel_session.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests_parallel_session PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_parallel_session COMMAND cutwidth_tests_parallel_session)
    add_executable(cutwidth_tests_global_dfs_executor tests_global_dfs_executor.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests_global_dfs_executor PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_global_dfs_executor COMMAND cutwidth_tests_global_dfs_executor)
    add_executable(cutwidth_tests_adaptive_parallel_dfs tests_adaptive_parallel_dfs.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests_adaptive_parallel_dfs PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_adaptive_parallel_dfs COMMAND cutwidth_tests_adaptive_parallel_dfs)
    add_executable(cutwidth_tests_milp tests_milp.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests_milp PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_milp COMMAND cutwidth_tests_milp)
    add_executable(cutwidth_tests_pb tests_pb.cpp oracle.cpp)
    target_link_libraries(cutwidth_tests_pb PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_pb COMMAND cutwidth_tests_pb)
    if(CUTWIDTH_ENABLE_SDP_PROTOTYPE)
        add_executable(cutwidth_tests_sdp_operator tests_sdp_operator.cpp oracle.cpp)
        target_link_libraries(cutwidth_tests_sdp_operator PRIVATE cutwidth_solver)
        add_test(NAME cutwidth_tests_sdp_operator COMMAND cutwidth_tests_sdp_operator)
    endif()
    add_executable(cutwidth_tests_lagrangian tests_lagrangian.cpp)
    target_link_libraries(cutwidth_tests_lagrangian PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_lagrangian COMMAND cutwidth_tests_lagrangian)

    add_executable(cutwidth_fake_tool fake_tool.cpp)
    add_executable(cutwidth_tests_pb_sat_root tests_pb_sat_root.cpp)
    target_link_libraries(cutwidth_tests_pb_sat_root PRIVATE cutwidth_solver)
    add_dependencies(cutwidth_tests_pb_sat_root cutwidth_fake_tool)
    target_compile_definitions(cutwidth_tests_pb_sat_root PRIVATE
        FAKE_TOOL_PATH="$<TARGET_FILE:cutwidth_fake_tool>"
    )
    add_test(NAME cutwidth_tests_pb_sat_root COMMAND cutwidth_tests_pb_sat_root)

    add_executable(cutwidth_tests_pb_sat_embedded tests_pb_sat_embedded.cpp)
    target_link_libraries(cutwidth_tests_pb_sat_embedded PRIVATE cutwidth_solver)
    add_test(NAME cutwidth_tests_pb_sat_embedded COMMAND cutwidth_tests_pb_sat_embedded)
endif()

option(CUTWIDTH_BUILD_PYTHON "Build Python bindings using nanobind" OFF)
set(CUTWIDTH_BUNDLED_CADICAL "" CACHE FILEPATH "CaDiCaL executable bundled in Python wheels")
set(CUTWIDTH_BUNDLED_DRAT_TRIM "" CACHE FILEPATH "DRAT-trim executable bundled in Python wheels")
set(CUTWIDTH_BUNDLED_CADICAL_LICENSE "" CACHE FILEPATH "CaDiCaL license bundled in Python wheels")
set(CUTWIDTH_BUNDLED_DRAT_TRIM_LICENSE "" CACHE FILEPATH "DRAT-trim license bundled in Python wheels")
if(CUTWIDTH_BUILD_PYTHON)
    set_property(TARGET cutwidth_solver PROPERTY POSITION_INDEPENDENT_CODE ON)
    find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(nanobind CONFIG REQUIRED)
    nanobind_add_module(_boundedcuts bindings.cpp)
    target_link_libraries(_boundedcuts PRIVATE cutwidth_solver)

    install(TARGETS _boundedcuts
        LIBRARY DESTINATION boundedcuts
        RUNTIME DESTINATION boundedcuts)
    install(TARGETS cutwidth_exact RUNTIME DESTINATION boundedcuts)

    set(REQUIRED_PATHS CUTWIDTH_BUNDLED_CADICAL_LICENSE CUTWIDTH_BUNDLED_DRAT_TRIM_LICENSE)
    if(NOT WIN32)
        list(APPEND REQUIRED_PATHS CUTWIDTH_BUNDLED_CADICAL CUTWIDTH_BUNDLED_DRAT_TRIM)
    endif()
    foreach(required_path IN LISTS REQUIRED_PATHS)
        if("${${required_path}}" STREQUAL "" OR NOT EXISTS "${${required_path}}")
            message(FATAL_ERROR "${required_path} must name an existing file for Python wheels")
        endif()
    endforeach()
    if(NOT WIN32)
        install(PROGRAMS "${CUTWIDTH_BUNDLED_CADICAL}" "${CUTWIDTH_BUNDLED_DRAT_TRIM}"
            DESTINATION boundedcuts/bin)
    endif()
    install(FILES "${CUTWIDTH_BUNDLED_CADICAL_LICENSE}"
        DESTINATION boundedcuts/licenses RENAME CaDiCaL-LICENSE.txt)
    install(FILES "${CUTWIDTH_BUNDLED_DRAT_TRIM_LICENSE}"
        DESTINATION boundedcuts/licenses RENAME DRAT-trim-LICENSE.txt)
endif()
