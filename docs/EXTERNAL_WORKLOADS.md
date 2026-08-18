# External workload validation

This document records local validation of external repositories used as
candidate workloads for MicroErrorSystem. External code is not part of the
runtime API.

## nlohmann/json

- repository: `https://github.com/nlohmann/json`
- revision: `v3.12.0`
- framework integration: enabled through `BUILD_EXTERNAL_STRESS_TESTS=ON`
- workload: 20,000 JSON documents, 6,667 malformed inputs
- result: `4/4` CTest tests passed

Observed benchmark output:

```text
parse_only documents=20000 median_operations_per_second=108873
parse_and_error_control workers=4 documents=20000 median_operations_per_second=311158
```

## fmt

- repository: `https://github.com/fmtlib/fmt`
- revision: `11.2.0`
- local checkout: `third_party/workloads/fmt`
- intended workload: high-volume formatting and log-message generation

The Release CMake configuration completed, but compiling the library with the
installed LLVM-MinGW toolchain failed in fmt's `format.h` because `malloc` and
`free` were not visible at the template definition site. This is a third-party
toolchain compatibility issue, not a MicroErrorSystem failure. The workload
must be rechecked with GCC, MSVC, or a newer compatible fmt/toolchain pair
before it is promoted to a CI integration target.

## cpp-httplib

- repository: `https://github.com/yhirose/cpp-httplib`
- revision: `v0.18.0`
- local checkout: `third_party/workloads/cpp-httplib`
- intended workload: concurrent request handling and network error paths

The library configured successfully with tests and OpenSSL integration disabled
under the installed LLVM-MinGW toolchain. It is now integrated through the
optional `MicroErrorSystem.LocalExternalIntegrationTests` target. The test
starts a local server, sends 1,000 concurrent requests from four workers, uses
`fmt` to build failure messages, and registers 500 HTTP 503 responses through
`MultiThreadedSystem`.

Observed output:

```text
external_local_integration repository=fmt@11.2.0+cpp-httplib@v0.18.0 requests=1000 successful=500 recorded_failures=500
```

The target is local-only because the workload clones are intentionally ignored
by Git. Enable it with:

```powershell
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/build-local-integration `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_TESTING=ON `
  -DBUILD_LOCAL_EXTERNAL_INTEGRATIONS=ON
cmake --build MicroErrorSystem/build-local-integration --parallel
ctest --test-dir MicroErrorSystem/build-local-integration `
  -R LocalExternalIntegrationTests --output-on-failure
```

## spdlog

- repository: `https://github.com/gabime/spdlog`
- revision: `v1.15.3`
- local checkout: `third_party/workloads/spdlog`
- comparison target: `MicroErrorSystemSpdlogComparisonBenchmark`

The comparison uses spdlog's bundled fmt implementation and custom counting
and formatting sinks. It separates prepared-record dispatch from equal-output
formatting, and validates that both formatting paths consume the same byte
count. It does not mix the external fmt ABI with spdlog's bundled fmt. Build
and run it with:

```powershell
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/build-spdlog-compare `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_LOCAL_LIBRARY_COMPARISONS=ON
cmake --build MicroErrorSystem/build-spdlog-compare --parallel
MicroErrorSystem/build-spdlog-compare/MicroErrorSystemSpdlogComparisonBenchmark.exe
```

## Worker pool comparison

- `bshoshany/thread-pool`: `v5.1.0`, commit
  `bd4533f1f70c2b975cbd5769a60d8eaaea1d2233`;
- `taskflow/taskflow`: `v4.1.0`, commit
  `45366fe5bc4f2f8ec9aa590b40c504e296886865`;
- comparison target: `MicroErrorSystemWorkerPoolComparisonBenchmark`.

The target compares tracked futures and scalar fire-and-forget submission across
all three executors. It also compares the native bulk-container APIs available
in MicroErrorSystem and BS::thread_pool. All adapters execute and validate the
same 100,000 atomic increments on four workers.

```powershell
cmake --build MicroErrorSystem/build-spdlog-compare `
  --target MicroErrorSystemWorkerPoolComparisonBenchmark --parallel
MicroErrorSystem/build-spdlog-compare/MicroErrorSystemWorkerPoolComparisonBenchmark.exe
```

## Reproducibility

The shallow clones are local-only and ignored by Git. Recreate them with:

```powershell
git clone --depth 1 --branch v3.12.0 https://github.com/nlohmann/json.git third_party/workloads/nlohmann-json
git clone --depth 1 --branch 11.2.0 https://github.com/fmtlib/fmt.git third_party/workloads/fmt
git clone --depth 1 --branch v0.18.0 https://github.com/yhirose/cpp-httplib.git third_party/workloads/cpp-httplib
git clone --depth 1 --branch v5.1.0 https://github.com/bshoshany/thread-pool.git third_party/workloads/BS-thread-pool
git clone --depth 1 --branch v4.1.0 https://github.com/taskflow/taskflow.git third_party/workloads/taskflow
```
