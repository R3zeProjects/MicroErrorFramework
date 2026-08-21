cmake_minimum_required(VERSION 3.25)

project(MicroErrorSystem
    VERSION 0.2.3
    LANGUAGES CXX
)

set(MICRO_ERROR_SYSTEM_PRERELEASE "beta")

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(ENABLE_COVERAGE "Enable source coverage instrumentation" OFF)
option(BUILD_BENCHMARKS "Build microbenchmarks" OFF)
option(BUILD_FUZZERS "Build Clang LibFuzzer targets" OFF)
option(BUILD_FUZZ_SMOKE "Build a sanitizer-backed deterministic fuzz smoke test" OFF)
option(BUILD_EXTERNAL_STRESS_TESTS
    "Build stress tests against pinned third-party repositories"
    OFF
)
set(LOCAL_EXTERNAL_WORKLOAD_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/workloads"
    CACHE PATH "Local root containing pinned external workload checkouts"
)
option(BUILD_LOCAL_EXTERNAL_INTEGRATIONS
    "Build integrations against local fmt and cpp-httplib checkouts"
    OFF
)
option(BUILD_LOCAL_LIBRARY_COMPARISONS
    "Build benchmarks against local comparable libraries"
    OFF
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "#include <expected>
     #include <thread>
     int main() {
         std::expected<void, int> value;
         std::jthread worker{[](std::stop_token) {}};
         return value ? 0 : 1;
     }"
    VOSP_HAS_REQUIRED_CXX23_LIBRARY
)
if(NOT VOSP_HAS_REQUIRED_CXX23_LIBRARY)
    message(FATAL_ERROR
        "The selected standard library does not provide the required C++20/23 "
        "facilities (std::expected, std::jthread, and std::stop_token). Use GCC "
        "with a recent libstdc++, MSVC, or a libc++ version that implements them."
    )
endif()

add_library(MicroErrorSystemHeaders INTERFACE)
add_library(vosp::vosp ALIAS MicroErrorSystemHeaders)
set_target_properties(MicroErrorSystemHeaders PROPERTIES EXPORT_NAME vosp)
target_include_directories(MicroErrorSystemHeaders
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(MicroErrorSystemHeaders INTERFACE cxx_std_23)
target_link_libraries(MicroErrorSystemHeaders INTERFACE Threads::Threads)

install(TARGETS MicroErrorSystemHeaders
    EXPORT vospTargets
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT vospTargets
    FILE vospTargets.cmake
    NAMESPACE vosp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vosp
)
configure_package_config_file(
    cmake/vospConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/vospConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vosp
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/vospConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/vospConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/vospConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vosp
)

if(MSVC)
    # Cache-line isolation intentionally introduces tail padding in aligned
    # counters and sink shards; C4324 is therefore not actionable.
    target_compile_options(MicroErrorSystemHeaders INTERFACE /W4 /permissive- /wd4324)
else()
    target_compile_options(MicroErrorSystemHeaders INTERFACE -Wall -Wextra -Wpedantic)
endif()

add_executable(MicroErrorSystem main.cpp)
target_link_libraries(MicroErrorSystem PRIVATE MicroErrorSystemHeaders)

function(enable_sanitizers target)
    if(NOT ENABLE_SANITIZERS)
        if(NOT ENABLE_THREAD_SANITIZER)
            return()
        endif()
    endif()

    if(MSVC)
        message(FATAL_ERROR "ENABLE_SANITIZERS is not configured for MSVC in this project")
    endif()

    if(ENABLE_SANITIZERS AND ENABLE_THREAD_SANITIZER)
        message(FATAL_ERROR "ENABLE_SANITIZERS and ENABLE_THREAD_SANITIZER cannot be enabled together")
    endif()

    if(ENABLE_THREAD_SANITIZER)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    else()
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

function(enable_coverage target)
    if(NOT ENABLE_COVERAGE)
        return()
    endif()

    if(MSVC)
        message(FATAL_ERROR "ENABLE_COVERAGE requires GCC or Clang coverage tooling")
    endif()

    target_compile_options(${target} PRIVATE -O0 -g --coverage)
    target_link_options(${target} PRIVATE --coverage)
endfunction()

if(ENABLE_COVERAGE AND (ENABLE_SANITIZERS OR ENABLE_THREAD_SANITIZER))
    message(FATAL_ERROR
        "ENABLE_COVERAGE cannot be combined with sanitizer instrumentation"
    )
endif()

enable_sanitizers(MicroErrorSystem)

if(BUILD_EXTERNAL_STRESS_TESTS)
    include(FetchContent)

    # Pinned release used only as an integration workload; it is not a runtime dependency.
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(BUILD_LOCAL_EXTERNAL_INTEGRATIONS)
    set(LocalFmtIncludeDir "${LOCAL_EXTERNAL_WORKLOAD_ROOT}/fmt/include")
    set(LocalHttpLibIncludeDir "${LOCAL_EXTERNAL_WORKLOAD_ROOT}/cpp-httplib")
    if(NOT EXISTS "${LocalFmtIncludeDir}/fmt/format.h")
        message(FATAL_ERROR "fmt checkout not found under ${LOCAL_EXTERNAL_WORKLOAD_ROOT}")
    endif()
    if(NOT EXISTS "${LocalHttpLibIncludeDir}/httplib.h")
        message(FATAL_ERROR "cpp-httplib checkout not found under ${LOCAL_EXTERNAL_WORKLOAD_ROOT}")
    endif()

    add_executable(MicroErrorSystemLocalExternalIntegrationTests
        tests/vosp_external_local_integration_tests.cpp
    )
    target_link_libraries(MicroErrorSystemLocalExternalIntegrationTests
        PRIVATE MicroErrorSystemHeaders
    )
    if(WIN32)
        target_link_libraries(MicroErrorSystemLocalExternalIntegrationTests
            PRIVATE ws2_32
        )
    endif()
    if(NOT MSVC)
        target_compile_options(MicroErrorSystemLocalExternalIntegrationTests
            PRIVATE -Wno-deprecated-literal-operator
        )
    endif()
    target_include_directories(MicroErrorSystemLocalExternalIntegrationTests
        PRIVATE
            "${LocalFmtIncludeDir}"
            "${LocalHttpLibIncludeDir}"
    )
    target_compile_definitions(MicroErrorSystemLocalExternalIntegrationTests
        PRIVATE FMT_HEADER_ONLY=1
    )
    enable_sanitizers(MicroErrorSystemLocalExternalIntegrationTests)
endif()

if(BUILD_LOCAL_LIBRARY_COMPARISONS)
    set(LocalSpdlogIncludeDir "${LOCAL_EXTERNAL_WORKLOAD_ROOT}/spdlog/include")
    if(NOT EXISTS "${LocalSpdlogIncludeDir}/spdlog/logger.h")
        message(FATAL_ERROR "spdlog checkout not found under ${LOCAL_EXTERNAL_WORKLOAD_ROOT}")
    endif()

    add_executable(MicroErrorSystemSpdlogComparisonBenchmark
        benchmarks/vosp_spdlog_comparison_benchmark.cpp
    )
    target_link_libraries(MicroErrorSystemSpdlogComparisonBenchmark
        PRIVATE MicroErrorSystemHeaders
    )
    target_include_directories(MicroErrorSystemSpdlogComparisonBenchmark
        PRIVATE "${LocalSpdlogIncludeDir}"
    )
    enable_sanitizers(MicroErrorSystemSpdlogComparisonBenchmark)

    set(LocalBsThreadPoolIncludeDir
        "${LOCAL_EXTERNAL_WORKLOAD_ROOT}/BS-thread-pool/include"
    )
    set(LocalTaskflowIncludeDir
        "${LOCAL_EXTERNAL_WORKLOAD_ROOT}/taskflow"
    )
    if(NOT EXISTS "${LocalBsThreadPoolIncludeDir}/BS_thread_pool.hpp")
        message(FATAL_ERROR
            "BS::thread_pool checkout not found under ${LOCAL_EXTERNAL_WORKLOAD_ROOT}"
        )
    endif()
    if(NOT EXISTS "${LocalTaskflowIncludeDir}/taskflow/taskflow.hpp")
        message(FATAL_ERROR
            "Taskflow checkout not found under ${LOCAL_EXTERNAL_WORKLOAD_ROOT}"
        )
    endif()

    add_executable(MicroErrorSystemWorkerPoolComparisonBenchmark
        benchmarks/vosp_worker_pool_comparison_benchmark.cpp
    )
    target_link_libraries(MicroErrorSystemWorkerPoolComparisonBenchmark
        PRIVATE MicroErrorSystemHeaders
    )
    target_include_directories(MicroErrorSystemWorkerPoolComparisonBenchmark
        PRIVATE
            "${LocalBsThreadPoolIncludeDir}"
            "${LocalTaskflowIncludeDir}"
    )
    enable_sanitizers(MicroErrorSystemWorkerPoolComparisonBenchmark)
endif()

if(BUILD_TESTING)
    add_executable(MicroErrorSystemTests tests/vosp_error_tests.cpp)
    target_link_libraries(MicroErrorSystemTests PRIVATE MicroErrorSystemHeaders)

    add_executable(MicroErrorSystemLoggerTests tests/vosp_logger_tests.cpp)
    target_link_libraries(MicroErrorSystemLoggerTests PRIVATE MicroErrorSystemHeaders)

    add_executable(MicroErrorSystemWorkerPoolStressTests
        tests/vosp_worker_pool_stress_tests.cpp
    )
    target_link_libraries(MicroErrorSystemWorkerPoolStressTests
        PRIVATE MicroErrorSystemHeaders
    )

    add_executable(MicroErrorSystemConcurrencyContractTests
        tests/vosp_concurrency_contract_tests.cpp
    )
    target_link_libraries(MicroErrorSystemConcurrencyContractTests
        PRIVATE MicroErrorSystemHeaders
    )

    add_executable(MicroErrorSystemUnifiedApiTests
        tests/vosp_unified_api_tests.cpp
    )
    target_link_libraries(MicroErrorSystemUnifiedApiTests
        PRIVATE MicroErrorSystemHeaders
    )

    add_executable(MicroErrorSystemNegativeApiTests
        tests/vosp_negative_api_tests.cpp
    )
    target_link_libraries(MicroErrorSystemNegativeApiTests
        PRIVATE MicroErrorSystemHeaders
    )

    function(add_compile_fail_contract target source)
        add_executable(${target} EXCLUDE_FROM_ALL ${source})
        target_link_libraries(${target} PRIVATE MicroErrorSystemHeaders)

        set(test_name "MicroErrorSystem.CompileFail.${target}")
        add_test(
            NAME ${test_name}
            COMMAND ${CMAKE_COMMAND}
                    --build ${CMAKE_BINARY_DIR}
                    --config $<CONFIG>
                    --target ${target}
        )
        set_tests_properties(${test_name} PROPERTIES
            WILL_FAIL TRUE
            RUN_SERIAL TRUE
            LABELS "api-contract;compile-fail"
            TIMEOUT 120
        )
    endfunction()

    add_compile_fail_contract(
        InvalidRegisterPolicy
        tests/compile_fail/invalid_register_policy.cpp
    )
    add_compile_fail_contract(
        InvalidAsyncExecutor
        tests/compile_fail/invalid_async_executor.cpp
    )
    add_compile_fail_contract(
        InvalidSinkPolicy
        tests/compile_fail/invalid_sink_policy.cpp
    )
    add_compile_fail_contract(
        InvalidLoggerDispatchPolicy
        tests/compile_fail/invalid_logger_dispatch_policy.cpp
    )

    if(BUILD_EXTERNAL_STRESS_TESTS)
        add_executable(MicroErrorSystemNlohmannStressTests
            tests/vosp_nlohmann_stress_tests.cpp
        )
        target_link_libraries(MicroErrorSystemNlohmannStressTests
            PRIVATE MicroErrorSystemHeaders nlohmann_json::nlohmann_json
        )
    endif()

    if(BUILD_LOCAL_EXTERNAL_INTEGRATIONS)
        add_test(
            NAME MicroErrorSystem.LocalExternalIntegrationTests
            COMMAND MicroErrorSystemLocalExternalIntegrationTests
        )
    endif()

    enable_sanitizers(MicroErrorSystemTests)
    enable_sanitizers(MicroErrorSystemLoggerTests)
    enable_sanitizers(MicroErrorSystemWorkerPoolStressTests)
    enable_sanitizers(MicroErrorSystemConcurrencyContractTests)
    enable_sanitizers(MicroErrorSystemUnifiedApiTests)
    enable_sanitizers(MicroErrorSystemNegativeApiTests)
    enable_coverage(MicroErrorSystemTests)
    enable_coverage(MicroErrorSystemLoggerTests)
    enable_coverage(MicroErrorSystemWorkerPoolStressTests)
    enable_coverage(MicroErrorSystemConcurrencyContractTests)
    enable_coverage(MicroErrorSystemUnifiedApiTests)
    enable_coverage(MicroErrorSystemNegativeApiTests)
    if(BUILD_EXTERNAL_STRESS_TESTS)
        enable_sanitizers(MicroErrorSystemNlohmannStressTests)
        enable_coverage(MicroErrorSystemNlohmannStressTests)
    endif()

    add_test(
        NAME MicroErrorSystem.Tests
        COMMAND MicroErrorSystemTests
    )

    add_test(
        NAME MicroErrorSystem.LoggerTests
        COMMAND MicroErrorSystemLoggerTests
    )

    add_test(
        NAME MicroErrorSystem.WorkerPoolStressTests
        COMMAND MicroErrorSystemWorkerPoolStressTests
    )

    add_test(
        NAME MicroErrorSystem.ConcurrencyContractTests
        COMMAND MicroErrorSystemConcurrencyContractTests
    )
    add_test(
        NAME MicroErrorSystem.UnifiedApiTests
        COMMAND MicroErrorSystemUnifiedApiTests
    )
    add_test(
        NAME MicroErrorSystem.NegativeApiTests
        COMMAND MicroErrorSystemNegativeApiTests
    )
    set_tests_properties(
        MicroErrorSystem.UnifiedApiTests
        MicroErrorSystem.NegativeApiTests
        PROPERTIES LABELS "api-contract"
    )
    set_tests_properties(MicroErrorSystem.ConcurrencyContractTests
        PROPERTIES TIMEOUT 120
    )

    if(BUILD_EXTERNAL_STRESS_TESTS)
        add_test(
            NAME MicroErrorSystem.NlohmannStressTests
            COMMAND MicroErrorSystemNlohmannStressTests
        )
    endif()

    # Keep the process environment intact and prepend toolchain DLL locations.
    # ENVIRONMENT previously received a semicolon-separated CMake list, which
    # CTest interpreted as several environment entries on Windows.
    if(WIN32)
        get_filename_component(MicroErrorSystemCompilerBin
            "${CMAKE_CXX_COMPILER}"
            DIRECTORY
        )
        set(MicroErrorSystemTestPathModification
            "PATH=path_list_prepend:${MicroErrorSystemCompilerBin}"
        )

        # Clang on Windows keeps ASan/UBSan DLLs under lib/clang/<version>.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            get_filename_component(MicroErrorSystemClangRoot
                "${MicroErrorSystemCompilerBin}"
                DIRECTORY
            )
            file(GLOB MicroErrorSystemClangRuntimeDirs
                "${MicroErrorSystemClangRoot}/lib/clang/*/lib/windows"
            )
            if(MicroErrorSystemClangRuntimeDirs)
                list(SORT MicroErrorSystemClangRuntimeDirs)
                list(GET MicroErrorSystemClangRuntimeDirs -1 MicroErrorSystemClangRuntime)
                list(APPEND MicroErrorSystemTestPathModification
                    "PATH=path_list_prepend:${MicroErrorSystemClangRuntime}"
                )
            endif()
        endif()

        set_tests_properties(
            MicroErrorSystem.Tests
            MicroErrorSystem.LoggerTests
            MicroErrorSystem.WorkerPoolStressTests
            MicroErrorSystem.ConcurrencyContractTests
            MicroErrorSystem.UnifiedApiTests
            MicroErrorSystem.NegativeApiTests
            PROPERTIES
                ENVIRONMENT_MODIFICATION "${MicroErrorSystemTestPathModification}"
        )
        if(BUILD_LOCAL_EXTERNAL_INTEGRATIONS)
            set_tests_properties(
                MicroErrorSystem.LocalExternalIntegrationTests
                PROPERTIES
                    ENVIRONMENT_MODIFICATION "${MicroErrorSystemTestPathModification}"
            )
        endif()
    endif()

    if(BUILD_EXTERNAL_STRESS_TESTS)
        if(WIN32)
            set_tests_properties(
                MicroErrorSystem.NlohmannStressTests
                PROPERTIES
                    ENVIRONMENT_MODIFICATION "${MicroErrorSystemTestPathModification}"
            )
        endif()
    endif()
endif()

if(BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "BUILD_FUZZERS requires a Clang compiler with LibFuzzer")
    endif()

    if(WIN32)
        message(FATAL_ERROR
            "BUILD_FUZZERS requires a Unix Clang toolchain; use Linux or macOS"
        )
    endif()

    add_executable(MicroErrorSystemFuzzer fuzz/vosp_error_fuzzer.cpp)
    target_link_libraries(MicroErrorSystemFuzzer PRIVATE MicroErrorSystemHeaders)
    target_compile_options(MicroErrorSystemFuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(MicroErrorSystemFuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
    )
endif()

if(BUILD_FUZZ_SMOKE)
    add_executable(MicroErrorSystemFuzzSmoke
        fuzz/vosp_error_fuzzer.cpp
        tests/vosp_fuzz_smoke.cpp
    )
    target_link_libraries(MicroErrorSystemFuzzSmoke PRIVATE MicroErrorSystemHeaders)
    if(MSVC)
        message(FATAL_ERROR "BUILD_FUZZ_SMOKE requires Clang or GCC sanitizer flags")
    endif()
    target_compile_options(MicroErrorSystemFuzzSmoke PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(MicroErrorSystemFuzzSmoke PRIVATE
        -fsanitize=address,undefined
    )
endif()

if(BUILD_BENCHMARKS)
    add_executable(MicroErrorSystemProductionBenchmark
        benchmarks/vosp_production_benchmark.cpp
    )
    target_link_libraries(MicroErrorSystemProductionBenchmark
        PRIVATE MicroErrorSystemHeaders
    )
    enable_sanitizers(MicroErrorSystemProductionBenchmark)
    if(BUILD_TESTING)
        add_test(
            NAME MicroErrorSystem.ProductionBenchmarkSmoke
            COMMAND MicroErrorSystemProductionBenchmark
                    --profile=quick
                    --operations=1000
        )
        set_tests_properties(MicroErrorSystem.ProductionBenchmarkSmoke
            PROPERTIES TIMEOUT 120
        )
    endif()

    add_executable(MicroErrorSystemBenchmark benchmarks/vosp_benchmark.cpp)
    target_link_libraries(MicroErrorSystemBenchmark PRIVATE MicroErrorSystemHeaders)
    enable_sanitizers(MicroErrorSystemBenchmark)

    add_executable(MicroErrorSystemComparisonBenchmark
        benchmarks/vosp_comparison_benchmark.cpp
    )
    target_link_libraries(MicroErrorSystemComparisonBenchmark
        PRIVATE MicroErrorSystemHeaders
    )
    enable_sanitizers(MicroErrorSystemComparisonBenchmark)

    add_executable(MicroErrorSystemApiSurfaceBenchmark
        benchmarks/vosp_api_surface_benchmark.cpp
    )
    target_link_libraries(MicroErrorSystemApiSurfaceBenchmark
        PRIVATE MicroErrorSystemHeaders
    )
    enable_sanitizers(MicroErrorSystemApiSurfaceBenchmark)

    if(BUILD_EXTERNAL_STRESS_TESTS)
        add_executable(MicroErrorSystemExternalBenchmark
            benchmarks/vosp_external_benchmark.cpp
        )
        target_link_libraries(MicroErrorSystemExternalBenchmark
            PRIVATE MicroErrorSystemHeaders nlohmann_json::nlohmann_json
        )
        enable_sanitizers(MicroErrorSystemExternalBenchmark)
    endif()
endif()
