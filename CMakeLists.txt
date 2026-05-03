cmake_minimum_required(VERSION 3.20)
project(haiku_act_runner CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Version ──────────────────────────────────────────────────────────────────
# Override with -DRUNNER_VERSION=x.y.z on the cmake command line (e.g. from
# the haikuporter recipe).  Falls back to "0.1.0-dev" for local builds.
if(NOT DEFINED RUNNER_VERSION OR RUNNER_VERSION STREQUAL "")
    set(RUNNER_VERSION "0.2.0")
endif()
message(STATUS "Runner version: ${RUNNER_VERSION}")
add_compile_definitions(RUNNER_VERSION="${RUNNER_VERSION}")

# ─── Compiler flags ────────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)

# ─── Platform detection ───────────────────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
    set(HAIKU TRUE)
    message(STATUS "Building for Haiku OS")
    # Haiku packages land under /system/develop; help CMake find them
    list(APPEND CMAKE_PREFIX_PATH
        /system/develop
        /boot/home/config/develop
    )
    # pkg-config search path
    set(ENV{PKG_CONFIG_PATH}
        "/system/develop/lib/pkgconfig:/boot/home/config/develop/lib/pkgconfig")
endif()

# ─── Find dependencies ────────────────────────────────────────────────────────

# libcurl — HTTP REST (registration) + Connect-RPC fallback transport
find_package(CURL REQUIRED)

# yaml-cpp — YAML config + workflow parsing
find_package(yaml-cpp REQUIRED)

# libmicrohttpd — embedded HTTP server for the local cache API.
# Optional: if not found we skip CacheServer entirely.
find_library(MICROHTTPD_LIBRARY NAMES microhttpd)
find_path(MICROHTTPD_INCLUDE_DIR NAMES microhttpd.h)
if(MICROHTTPD_LIBRARY AND MICROHTTPD_INCLUDE_DIR)
    message(STATUS "libmicrohttpd found (${MICROHTTPD_LIBRARY}) — building CacheServer")
    set(HAVE_MICROHTTPD TRUE)
else()
    message(STATUS "libmicrohttpd NOT found — CacheServer will not be built")
    set(HAVE_MICROHTTPD FALSE)
endif()

# nlohmann/json — bundled as header-only in vendor/
# We bundle it so there's no pkg dependency.
set(JSON_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/vendor/nlohmann")

# protobuf — message serialization
# Used even in the Connect-RPC / libcurl transport path for encode/decode.
find_package(Protobuf REQUIRED)

# gRPC (optional) — if available, use the native gRPC channel;
# otherwise fall back to Connect-RPC over libcurl.
find_package(gRPC QUIET)
if(gRPC_FOUND)
    message(STATUS "gRPC found — using native gRPC transport")
    set(USE_GRPC TRUE)
else()
    message(STATUS "gRPC NOT found — using Connect-RPC / libcurl transport")
    set(USE_GRPC FALSE)
endif()

# OpenSSL / libcrypto — used by ExprEvaluator::hashFiles() for SHA-256
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")

# ─── Protobuf generated sources (optional — skip if not yet generated) ────────
# Run:  protoc --cpp_out=proto/ proto/runner.proto
# to regenerate these files.
set(PROTO_SRCS "")
set(PROTO_HDRS "")

if(EXISTS "${CMAKE_SOURCE_DIR}/proto/runner.pb.cc")
    list(APPEND PROTO_SRCS proto/runner.pb.cc)
    list(APPEND PROTO_HDRS proto/runner.pb.h)
    message(STATUS "proto/runner.pb.cc found — including generated protobuf stubs")
else()
    message(STATUS "proto/runner.pb.cc NOT found — using hand-coded proto encode/decode")
    message(STATUS "  To generate: protoc --cpp_out=proto/ proto/runner.proto")
endif()

if(USE_GRPC AND EXISTS "${CMAKE_SOURCE_DIR}/proto/runner.grpc.pb.cc")
    list(APPEND PROTO_SRCS proto/runner.grpc.pb.cc)
    list(APPEND PROTO_HDRS proto/runner.grpc.pb.h)
endif()

# ─── Runner source files ──────────────────────────────────────────────────────
set(RUNNER_SRCS
    src/main.cpp

    src/config/Config.cpp
    src/config/RunnerState.cpp

    src/client/GiteaClient.cpp
    src/client/RunnerClient.cpp

    src/runner/Poller.cpp
    src/runner/TaskExecutor.cpp
    src/runner/StepRunner.cpp
    src/runner/LogForwarder.cpp
    src/runner/JobGraph.cpp
    src/runner/WorkflowOrchestrator.cpp

    src/process/ProcessSpawner.cpp
    src/process/EnvManager.cpp

    src/workflow/WorkflowParser.cpp
    src/workflow/ExprEvaluator.cpp
    src/workflow/ContextBuilder.cpp
    src/workflow/MatrixExpander.cpp

    src/action/ActionRunner.cpp

    src/util/Logger.cpp
)

if(HAVE_MICROHTTPD)
    list(APPEND RUNNER_SRCS src/cache/CacheServer.cpp)
endif()

# ─── Main executable ──────────────────────────────────────────────────────────
add_executable(act_runner
    ${RUNNER_SRCS}
)

target_include_directories(act_runner PRIVATE
    src/
    proto/
    ${JSON_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
)

if(HAVE_MICROHTTPD)
    target_include_directories(act_runner PRIVATE ${MICROHTTPD_INCLUDE_DIR})
    target_link_libraries(act_runner PRIVATE ${MICROHTTPD_LIBRARY})
    target_compile_definitions(act_runner PRIVATE HAVE_MICROHTTPD=1)
endif()

if(USE_GRPC)
    target_compile_definitions(act_runner PRIVATE USE_GRPC=1)
endif()

# ─── Link libraries ───────────────────────────────────────────────────────────
target_link_libraries(act_runner PRIVATE
    CURL::libcurl
    yaml-cpp
    ${Protobuf_LIBRARIES}
    OpenSSL::Crypto
    pthread
)

if(USE_GRPC)
    target_link_libraries(act_runner PRIVATE
        gRPC::grpc++
        gRPC::grpc++_reflection
    )
endif()

# Haiku-specific libraries
if(HAIKU)
    target_link_libraries(act_runner PRIVATE
        network  # Haiku socket/network library
        be       # Haiku Application Kit (find_directory etc.)
    )

    # ── Icon embedding ─────────────────────────────────────────────────────
    # Compile the HVIF rdef to a resource file and embed it into the binary.
    # The icon appears in Tracker, the Deskbar, and HaikuDepot.
    find_program(RC_TOOL rc)
    find_program(XRES_TOOL xres)
    if(RC_TOOL AND XRES_TOOL)
        add_custom_command(
            OUTPUT  ${CMAKE_BINARY_DIR}/app_icon.rsrc
            COMMAND ${RC_TOOL} -o ${CMAKE_BINARY_DIR}/app_icon.rsrc
                    ${CMAKE_SOURCE_DIR}/haiku-packaging/app_icon.rdef
            DEPENDS ${CMAKE_SOURCE_DIR}/haiku-packaging/app_icon.rdef
            COMMENT "Compiling act_runner icon resources"
        )
        add_custom_target(act_runner_icon
            DEPENDS ${CMAKE_BINARY_DIR}/app_icon.rsrc
        )
        add_dependencies(act_runner act_runner_icon)
        add_custom_command(TARGET act_runner POST_BUILD
            COMMAND ${XRES_TOOL} -o $<TARGET_FILE:act_runner>
                    ${CMAKE_BINARY_DIR}/app_icon.rsrc
            COMMENT "Embedding icon into act_runner"
        )
        message(STATUS "Icon embedding: rc=${RC_TOOL}  xres=${XRES_TOOL}")
    else()
        message(STATUS "Icon embedding skipped (rc/xres not found)")
    endif()
endif()

# ─── Tests ────────────────────────────────────────────────────────────────────

enable_testing()

set(TEST_COMMON_SRCS
    src/workflow/ExprEvaluator.cpp
    src/workflow/WorkflowParser.cpp
    src/workflow/MatrixExpander.cpp
    src/workflow/ContextBuilder.cpp
    src/process/EnvManager.cpp
    src/process/ProcessSpawner.cpp
    src/config/Config.cpp
    src/config/RunnerState.cpp
    src/util/Logger.cpp
    src/runner/JobGraph.cpp
)

function(add_runner_test name src)
    add_executable(${name} tests/${src} ${TEST_COMMON_SRCS})
    target_include_directories(${name} PRIVATE
        src/ vendor/
        ${CURL_INCLUDE_DIRS}
        ${Protobuf_INCLUDE_DIRS}
    )
    target_link_libraries(${name} PRIVATE
        CURL::libcurl yaml-cpp ${Protobuf_LIBRARIES} OpenSSL::Crypto pthread
    )
    if(HAIKU)
        target_link_libraries(${name} PRIVATE network be)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_runner_test(test_expr_evaluator  test_expr_evaluator.cpp)
add_runner_test(test_workflow_parser test_workflow_parser.cpp)
add_runner_test(test_matrix_expander test_matrix_expander.cpp)
add_runner_test(test_env_manager     test_env_manager.cpp)
add_runner_test(test_process_spawner test_process_spawner.cpp)
add_runner_test(test_job_graph       test_job_graph.cpp)
add_runner_test(test_context_builder test_context_builder.cpp)

# test_runner_client needs RunnerClient (libcurl-dependent)
add_executable(test_runner_client
    tests/test_runner_client.cpp
    src/client/RunnerClient.cpp
    ${TEST_COMMON_SRCS}
)
target_include_directories(test_runner_client PRIVATE
    src/ vendor/
    ${CURL_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(test_runner_client PRIVATE
    CURL::libcurl yaml-cpp ${Protobuf_LIBRARIES} OpenSSL::Crypto pthread
)
if(HAIKU)
    target_link_libraries(test_runner_client PRIVATE network be)
endif()
add_test(NAME test_runner_client COMMAND test_runner_client)

# test_action_runner needs ActionRunner + LogForwarder + RunnerClient
add_executable(test_action_runner
    tests/test_action_runner.cpp
    src/action/ActionRunner.cpp
    src/runner/LogForwarder.cpp
    src/client/RunnerClient.cpp
    ${TEST_COMMON_SRCS}
)
target_include_directories(test_action_runner PRIVATE
    src/ vendor/
    ${CURL_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(test_action_runner PRIVATE
    CURL::libcurl yaml-cpp ${Protobuf_LIBRARIES} OpenSSL::Crypto pthread
)
if(HAIKU)
    target_link_libraries(test_action_runner PRIVATE network be)
endif()
add_test(NAME test_action_runner COMMAND test_action_runner)

# test_task_executor: full TaskExecutor integration test driven by
# MockRunnerClient (no network, no gitea).  Covers workflow parsing,
# expression interpolation, step outputs, matrix/needs context, log
# streaming, and failure semantics end-to-end.
add_executable(test_task_executor
    tests/test_task_executor.cpp
    src/runner/TaskExecutor.cpp
    src/runner/StepRunner.cpp
    src/runner/LogForwarder.cpp
    src/action/ActionRunner.cpp
    ${TEST_COMMON_SRCS}
)
target_include_directories(test_task_executor PRIVATE
    src/ vendor/
    ${CURL_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(test_task_executor PRIVATE
    CURL::libcurl yaml-cpp ${Protobuf_LIBRARIES} OpenSSL::Crypto pthread
)
if(HAIKU)
    target_link_libraries(test_task_executor PRIVATE network be)
endif()
add_test(NAME test_task_executor COMMAND test_task_executor)
# test_task_executor spawns real subprocesses; on Haiku with system load the
# output ordering can race between concurrent load_image() calls.
# Allow up to 3 CTest retries and increase timeout to tolerate this.
set_tests_properties(test_task_executor PROPERTIES
    TIMEOUT 120
)

# test_cache_server: starts a CacheServer on localhost + drives it with
# libcurl.  Requires libmicrohttpd.
if(HAVE_MICROHTTPD)
    add_executable(test_cache_server
        tests/test_cache_server.cpp
        src/cache/CacheServer.cpp
        src/util/Logger.cpp
    )
    target_include_directories(test_cache_server PRIVATE
        src/ vendor/
        ${CURL_INCLUDE_DIRS}
        ${MICROHTTPD_INCLUDE_DIR}
    )
    target_compile_definitions(test_cache_server PRIVATE HAVE_MICROHTTPD=1)
    target_link_libraries(test_cache_server PRIVATE
        CURL::libcurl ${MICROHTTPD_LIBRARY} pthread
    )
    if(HAIKU)
        target_link_libraries(test_cache_server PRIVATE network be)
    endif()
    add_test(NAME test_cache_server COMMAND test_cache_server)
endif()

# test_workflow_orchestrator: exercises multi-job wave dispatch using
# MockRunnerClient (no network).  Tests: single-job success/failure,
# linear chain, diamond, fail-fast wave skipping, callback.
add_executable(test_workflow_orchestrator
    tests/test_workflow_orchestrator.cpp
    src/runner/WorkflowOrchestrator.cpp
    src/runner/TaskExecutor.cpp
    src/runner/StepRunner.cpp
    src/runner/LogForwarder.cpp
    src/action/ActionRunner.cpp
    ${TEST_COMMON_SRCS}
)
target_include_directories(test_workflow_orchestrator PRIVATE
    src/ vendor/
    ${CURL_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(test_workflow_orchestrator PRIVATE
    CURL::libcurl yaml-cpp ${Protobuf_LIBRARIES} OpenSSL::Crypto pthread
)
if(HAIKU)
    target_link_libraries(test_workflow_orchestrator PRIVATE network be)
endif()
add_test(NAME test_workflow_orchestrator COMMAND test_workflow_orchestrator)

# test_poller: unit tests for the Poller dispatch loop, capacity semaphore,
# and graceful shutdown.  Uses ControlledMockClient (a gated IRunnerClient)
# so no real network is needed.  Tasks are trivial shell steps.
add_executable(test_poller
    tests/test_poller.cpp
    src/runner/Poller.cpp
    src/runner/TaskExecutor.cpp
    src/runner/StepRunner.cpp
    src/runner/LogForwarder.cpp
    src/action/ActionRunner.cpp
    ${TEST_COMMON_SRCS}
)
target_include_directories(test_poller PRIVATE
    src/ vendor/
    ${CURL_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(test_poller PRIVATE
    CURL::libcurl yaml-cpp ${Protobuf_LIBRARIES} OpenSSL::Crypto pthread
)
if(HAIKU)
    target_link_libraries(test_poller PRIVATE network be)
endif()
add_test(NAME test_poller COMMAND test_poller)

# test_main_cli: black-box integration tests that spawn the act_runner binary
# via popen() and check exit codes + output for CLI subcommands.
# ACT_RUNNER_BIN is set to the binary path so the test can locate it.
add_executable(test_main_cli
    tests/test_main_cli.cpp
)
target_include_directories(test_main_cli PRIVATE
    src/ vendor/
)
target_link_libraries(test_main_cli PRIVATE pthread)
if(HAIKU)
    target_link_libraries(test_main_cli PRIVATE network be)
endif()
add_test(NAME test_main_cli COMMAND test_main_cli)
# Pass the binary path via environment so the test finds the right act_runner.
set_tests_properties(test_main_cli PROPERTIES
    ENVIRONMENT "ACT_RUNNER_BIN=$<TARGET_FILE:act_runner>"
    DEPENDS act_runner
)

# ─── Haiku SIGKILLTHR mitigation ─────────────────────────────────────────────
# On Haiku, posix_spawn() can occasionally deliver SIGKILLTHR (signal 7) to a
# newly spawned child when the parent process has many live threads.  Tests
# that fork child processes are vulnerable when CTest runs several test
# binaries concurrently (CTest default: all jobs in parallel).
#
# Strategy:
#   1. Assign a shared RESOURCE_LOCK "process_spawn" to every test that uses
#      posix_spawn internally, so CTest never runs two such tests at the same
#      time.  This does NOT affect the parallelism of pure in-process tests.
#   2. Add RUN_SERIAL to the heaviest tests (task_executor,
#      workflow_orchestrator) that themselves spawn many threads + processes,
#      so they always run alone regardless of CTest -j level.
#
# The RESOURCE_LOCK approach keeps the overall suite fast (pure tests still
# parallelise) while preventing the cross-test SIGKILLTHR race.
set_tests_properties(
    test_process_spawner
    test_action_runner
    test_task_executor
    test_workflow_orchestrator
    test_poller
    test_main_cli
    PROPERTIES
        RESOURCE_LOCK "process_spawn"
)

# These three are the most thread-heavy; run them fully serialised.
set_tests_properties(
    test_task_executor
    test_workflow_orchestrator
    test_poller
    PROPERTIES
        RUN_SERIAL TRUE
)

include(GNUInstallDirs)

install(TARGETS act_runner
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES config.yaml.example
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    RENAME config.yaml.example
)

# ─── Summary ─────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "=== haiku-act-runner build configuration ===")
message(STATUS "  C++ standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Platform     : ${CMAKE_SYSTEM_NAME}")
if(USE_GRPC)
    message(STATUS "  Transport    : gRPC native")
else()
    message(STATUS "  Transport    : Connect-RPC/libcurl")
endif()
message(STATUS "  CURL version : ${CURL_VERSION_STRING}")
message(STATUS "  Protobuf ver : ${Protobuf_VERSION}")
message(STATUS "============================================")
message(STATUS "")
