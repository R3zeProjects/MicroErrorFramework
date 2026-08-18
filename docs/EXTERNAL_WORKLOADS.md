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
parse_only elapsed_us=178771 operations_per_second=111875
parse_and_error_control workers=4 elapsed_us=89050 operations_per_second=224593
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

## Reproducibility

The shallow clones are local-only and ignored by Git. Recreate them with:

```powershell
git clone --depth 1 --branch v3.12.0 https://github.com/nlohmann/json.git third_party/workloads/nlohmann-json
git clone --depth 1 --branch 11.2.0 https://github.com/fmtlib/fmt.git third_party/workloads/fmt
git clone --depth 1 --branch v0.18.0 https://github.com/yhirose/cpp-httplib.git third_party/workloads/cpp-httplib
```
