# MicroErrorSystem

**MicroErrorSystem** is a C++23 micro-framework for building a
system-independent error handling and logging contour.

It provides one public API for classifying, registering, routing, logging,
and asynchronously processing errors without coupling the application to a
particular operating system or domain.

> **Main idea:** an error-control and logging contour independent of the host system.

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![API](https://img.shields.io/badge/API-0.1.0-informational)
![CMake](https://img.shields.io/badge/CMake-3.25%2B-064F8C)
![License](https://img.shields.io/badge/license-MIT-green)

## Why this project exists

Application code often mixes domain logic, error storage, logging, thread
management, and shutdown handling. MicroErrorSystem separates these concerns
behind small interfaces and compile-time policies.

The framework is intentionally small and header-only. It is suitable for
experiments, infrastructure components, services, and as a reusable error
control layer in larger C++ systems.

## Features

- strongly typed `Error`, `Result<T>`, and `OperationResult` based on
  `std::expected`;
- category-specific registers with `IRegister` and `CategoryRegister`;
- `MemoryRegister` with thread-safe storage and bounded capacity;
- category routing through `Handler` and `ConcurrentHandler`;
- compile-time single-threaded, multi-threaded, and asynchronous systems;
- `IndustrialWorkerPool` with bounded workers and bounded queue;
- blocking backpressure when the queue is full;
- cooperative cancellation with `std::stop_token`;
- queue cleanup and configurable drain/cancel shutdown behavior;
- extensible logger with custom sinks and compile-time filtering policies;
- RAII-based ownership and safe asynchronous lifetime handling;
- CTest, stress tests, AddressSanitizer, UndefinedBehaviorSanitizer,
  ThreadSanitizer, LibFuzzer, Valgrind, coverage, and Callgrind workflows;
- external integration workloads using pinned third-party repositories.

## Measured results

All values below are real local Release measurements, not theoretical claims.

| Workload | Result |
| --- | ---: |
| Single-threaded register inserts | **3.6324M operations/s** |
| Multi-threaded system, 3 workers | **4.53325M operations/s** |
| Async system, 1,000 operations | **244,678 operations/s** |
| `nlohmann/json`, parse-only, 20,000 documents | **111,875 documents/s** |
| `nlohmann/json`, parse + error control, 4 workers | **224,593 documents/s** |
| `fmt` + `cpp-httplib` integration | **1,000 requests; 500 errors routed** |
| MicroErrorSystem logger, single-threaded | **3.73958M records/s** |
| `spdlog` `null_sink`, single-threaded | **15.3988M records/s** |
| MicroErrorSystem logger, 4 workers | **2.18441M records/s** |
| `spdlog` `null_sink`, 4 workers | **24.2072M records/s** |

Verification results:

```text
Native CTest                         3/3 passed
nlohmann/json external CTest        4/4 passed
fmt + cpp-httplib local CTest       4/4 passed
Windows sanitizer fuzz smoke       100,000 inputs passed
```

The benchmark machine was an AMD Ryzen 7 PRO 1700X with 8 physical cores,
16 logical processors, 31.95 GiB RAM, Windows 10 Pro, and Clang 22.1.6.
Detailed methodology and baseline comparisons are available in
[docs/BENCHMARKS.md](docs/BENCHMARKS.md).

The logger comparison uses `spdlog v1.15.3` with its `null_sink` and the same
100,000 formatted records. `spdlog` is faster in this narrow sink-throughput
workload. The result is expected: MicroErrorSystem additionally constructs a
typed `Error`, `LogEntry`, timestamp, thread id, and executes a user sink
callback. This is a component comparison, not a claim that one complete
framework replaces the other.

The logger hot path now has two targeted optimizations: a no-allocation fast
path for a single sink and an owned-error overload that moves temporary error
messages into `LogEntry`. The latest run is recorded above; throughput remains
machine-dependent.

## Architecture

```text
Error
  │
  ▼
Handler ─────► CategoryRegister ─────► Error storage
  │
  ├──────────► Result<T> / OperationResult
  ├──────────► Logger ─────► Sink
  └──────────► AsyncSystem ─────► IndustrialWorkerPool
```

The system mode is selected by types rather than runtime flags:

```cpp
using Single = vosp::error::SingleThreadedSystem<RegisterA, RegisterB>;
using Multi  = vosp::error::MultiThreadedSystem<RegisterA, RegisterB>;
using Async  = vosp::error::AsyncSystem<Executor, RegisterA, RegisterB>;
```

## Why policies are needed

Policies keep execution and logging decisions explicit at compile time:

- `TypeRegister` selects the storage/synchronization model;
- `TypeHandler` selects routing behavior for that model;
- `LoggerPolicy` decides which log levels reach sinks;
- `MinimumLevelPolicy<Level::WARNING>` removes lower-severity records before
  sink callbacks, reducing work and output volume.

This avoids runtime mode switches in hot paths and lets the compiler reject an
invalid policy type. Policies are intentionally small stateless types, so they
do not add per-object configuration storage. They should describe behavior,
not own resources; ownership remains explicit in registers, sinks, executors,
and worker pools.

Registers and logger sinks are non-owning dependencies. They must outlive the
objects that use them. The built-in asynchronous system keeps its handler state
alive for submitted operations so destroying the system does not invalidate
pending futures.

## Requirements

- CMake 3.25 or newer;
- a compiler with C++23 support;
- a standard library implementation providing `std::expected`;
- Git for optional external integration workloads.

The project is tested on Windows and Ubuntu in GitHub Actions. The LibFuzzer
configuration targets Unix-like platforms with Clang.

## Quick start

From the repository root:

```bash
cmake -S MicroErrorSystem -B MicroErrorSystem/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build MicroErrorSystem/build --parallel
ctest --test-dir MicroErrorSystem/build --output-on-failure
```

On Windows, the executable suffix is `.exe`.

## Minimal example

```cpp
#include <vosp.hpp>

using namespace vosp::error;

MemoryRegister<Category::NETWORK> network;
SingleThreadedSystem<decltype(network)> system{network};

const Error error{
    Category::NETWORK,
    1001,
    "Connection refused"
};

const OperationResult result = system.add(error);
if (!result) {
    // The register rejected the error or no matching category was found.
    const Error& reason = result.error();
}

if (network.contains(error)) {
    system.remove(error);
}
```

For value-returning operations:

```cpp
Result<int> read_attempts()
{
    return 3;
}

const Result<int> result = read_attempts();
if (result) {
    const int attempts = *result;
}
```

## Industrial worker pool

```cpp
#include <vosp.hpp>

vosp::async::IndustrialWorkerPool pool{
    4,    // worker count
    128   // queue capacity
};

auto future = pool.submit([] {
    return vosp::error::OperationResult{};
});

const vosp::error::OperationResult result = future.get();
pool.shutdown(vosp::async::ShutdownMode::DRAIN);
```

The hard limits are 1,024 workers and 1,024 queued tasks. `submit()` applies
blocking backpressure. `clear_queue()` completes pending futures with a
cancellation error. A running task is not forcefully interrupted; use
`submit_cancellable()` and inspect its `std::stop_token` for cooperative
cancellation.

## Logging

```cpp
#include <vosp.hpp>
#include <iostream>

vosp::logger::ConsoleSink console{std::cout};
vosp::logger::Logger logger{console};

logger.error(vosp::error::Error{
    vosp::error::Category::NETWORK,
    1001,
    "Connection refused"
});
```

Custom sinks implement `ILogSink`. `PolicyLogger` supports compile-time
filtering, for example `MinimumLevelPolicy<Level::WARNING>`. Sink ownership is
external and must outlive the logger.

## Build profiles

### Benchmarks

```bash
cmake -S MicroErrorSystem -B MicroErrorSystem/build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BENCHMARKS=ON
cmake --build MicroErrorSystem/build-bench --parallel
MicroErrorSystem/build-bench/MicroErrorSystemBenchmark
```

### AddressSanitizer and UndefinedBehaviorSanitizer

```bash
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DENABLE_SANITIZERS=ON
cmake --build MicroErrorSystem/build-asan --parallel
ctest --test-dir MicroErrorSystem/build-asan --output-on-failure
```

### ThreadSanitizer

```bash
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DENABLE_THREAD_SANITIZER=ON
cmake --build MicroErrorSystem/build-tsan --parallel
ctest --test-dir MicroErrorSystem/build-tsan --output-on-failure
```

### Coverage

```bash
cmake -S MicroErrorSystem -B MicroErrorSystem/build-coverage \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DENABLE_COVERAGE=ON
cmake --build MicroErrorSystem/build-coverage --parallel
ctest --test-dir MicroErrorSystem/build-coverage --output-on-failure
gcovr --root MicroErrorSystem \
  --filter 'MicroErrorSystem/include/.*' \
  --txt --xml-pretty --output coverage.xml
```

Coverage is a diagnostic signal. Thread-safety and lifecycle correctness are
also checked with stress tests, ThreadSanitizer, and targeted unit tests.

### LibFuzzer

```bash
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/build-fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_FUZZERS=ON
cmake --build MicroErrorSystem/build-fuzz --parallel
MicroErrorSystem/build-fuzz/MicroErrorSystemFuzzer -runs=10000
```

For Windows or toolchains without a compatible LibFuzzer runtime, the
repository also provides a sanitizer-backed deterministic smoke target:

```bash
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/build-fuzz-smoke \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_FUZZ_SMOKE=ON \
  -DBUILD_TESTING=OFF
cmake --build MicroErrorSystem/build-fuzz-smoke --parallel
MicroErrorSystem/build-fuzz-smoke/MicroErrorSystemFuzzSmoke
```

The smoke target executes 100,000 generated inputs through the same fuzz
harness under AddressSanitizer and UndefinedBehaviorSanitizer. It complements
the coverage-guided LibFuzzer job used on Ubuntu CI.

## Real benchmark results

The following results were collected from a local Release build on:

- Windows 10 Pro;
- AMD Ryzen 7 PRO 1700X, 8 physical cores / 16 logical processors;
- 31.95 GiB RAM;
- Clang 22.1.6;
- benchmark date: 2026-08-17.

Native API workload:

```text
single operations=100000 elapsed_us=27530 operations_per_second=3.6324e+06
multi workers=3 operations=99999 elapsed_us=22059 operations_per_second=4.53325e+06
async operations=1000 elapsed_us=4087 operations_per_second=244678
```

External integration workload using the official `nlohmann/json` repository,
pinned to `v3.12.0`:

```text
external_repo=nlohmann/json@v3.12.0 documents=20000 malformed=6667
parse_only elapsed_us=178771 operations_per_second=111875
parse_and_error_control workers=4 elapsed_us=89050 operations_per_second=224593
```

A later verification run on the same machine produced:

```text
parse_only elapsed_us=265600 operations_per_second=75301.2
parse_and_error_control workers=4 elapsed_us=83821 operations_per_second=238604
```

These are reproducible local measurements, not performance guarantees. Results
depend on CPU frequency policy, compiler, build mode, dependency version,
workload composition, and background processes. Full methodology is available
in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

### Baseline comparison

The repository also compares the public `MemoryRegister` path with a raw
`std::unordered_set<Error, ErrorHash>` baseline using the same 100,000-insert
workload. One additional Release run produced:

```text
raw_unordered_set operations_per_second=4.85437e+06
memory_register operations=100000 operations_per_second=4.97711e+06
raw_mutex_sets operations_per_second=8.09512e+06
memory_register_parallel operations_per_second=1.30309e+07
```

The baseline is intentionally low-level and does not provide category
validation, capacity errors, `Result` propagation, routing, or lifecycle
contracts. The numbers therefore describe overhead under this workload; they
do not establish that the framework is universally faster than a raw container.
Run it with `-DBUILD_BENCHMARKS=ON` using the
`MicroErrorSystemComparisonBenchmark` target.

### External HTTP integration workload

The local integration combines `fmt 11.2.0`, `cpp-httplib v0.18.0`, and
MicroErrorSystem's `MultiThreadedSystem`. Four workers issue requests against a
local HTTP server and route failed responses into a network error register.

```text
external_local_integration repository=fmt@11.2.0+cpp-httplib@v0.18.0
requests=1000 successful=500 recorded_failures=500
```

The workload passed as `4/4` CTest tests in the local integration profile.
It measures integration correctness and concurrent error routing; it is not a
general-purpose HTTP server benchmark.

## External repository testing

The current integration test uses the official MIT-licensed
[`nlohmann/json`](https://github.com/nlohmann/json) repository as a real parser
workload. It processes 20,000 documents, classifies malformed input, and routes
the resulting errors through the framework. The third-party library is used only
by the optional test and benchmark targets; it is not part of the runtime API.

Configure and run it with:

```bash
cmake -S MicroErrorSystem -B MicroErrorSystem/build-external \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARKS=ON \
  -DBUILD_EXTERNAL_STRESS_TESTS=ON
cmake --build MicroErrorSystem/build-external --parallel
ctest --test-dir MicroErrorSystem/build-external --output-on-failure
MicroErrorSystem/build-external/MicroErrorSystemExternalBenchmark
```

Additional external workloads can be placed under `third_party/workloads` and
connected through optional CMake targets. They should be pinned to a tag or
commit and must remain outside the public runtime dependency graph.

The local workload set currently contains shallow clones of:

| Repository | Revision | Intended workload |
| --- | --- | --- |
| [`nlohmann/json`](https://github.com/nlohmann/json) | `v3.12.0` | malformed-input classification and JSON parsing |
| [`fmtlib/fmt`](https://github.com/fmtlib/fmt) | `11.2.0` | high-throughput formatting and log-message generation |
| [`yhirose/cpp-httplib`](https://github.com/yhirose/cpp-httplib) | `v0.18.0` | concurrent request/error-path workload |
| [`gabime/spdlog`](https://github.com/gabime/spdlog) | `v1.15.3` | logger throughput comparison |

The `nlohmann/json` workload is wired into the external CMake stress target.
The `fmt` and `cpp-httplib` repositories are wired into an opt-in local HTTP
integration target. Their source trees are ignored by Git and are not shipped
as runtime dependencies.

## Verification status

The project currently includes:

- native Release tests: `3/3` passed;
- external integration tests: `4/4` passed;
- local `fmt` + `cpp-httplib` integration: `4/4` passed;
- Clang ASan/UBSan tests: `3/3` passed;
- extended C++23 warning and syntax checks without errors;
- LibFuzzer smoke test configuration;
- Valgrind Memcheck and Helgrind CI jobs;
- GCC coverage and Callgrind CI artifacts;
- Windows and Ubuntu CI build/test matrix.

## Repository layout

```text
MicroErrorSystem/
├── include/
│   ├── vosp.hpp                 # single public entry point
│   ├── vosp_error.hpp           # Error, Result, registers and systems
│   ├── vosp_logger.hpp          # logger, policies and sinks
│   └── vosp_worker_pool.hpp     # bounded asynchronous worker pool
├── tests/                       # unit, concurrency and external stress tests
├── benchmarks/                  # native and third-party benchmarks
├── fuzz/                        # LibFuzzer target
├── docs/
│   ├── ARCHITECTURE.md
│   └── BENCHMARKS.md
├── CMakeLists.txt
└── .github/workflows/ci.yml
```

## Current limitations

- The API is version `0.1.0` and may evolve before `1.0.0`.
- Registers and logger sinks are non-owning dependencies.
- A running worker task cannot be forcefully interrupted safely.
- `Category::NONE` is not routed to a specialized category register.
- `Error` stores its message in `std::string`, so predefined errors are
  `inline const` values rather than `inline constexpr` objects.
- Benchmark values are machine-dependent and should be compared only under the
  same toolchain and workload.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Benchmark report](docs/BENCHMARKS.md)
- [External workload validation](docs/EXTERNAL_WORKLOADS.md)
- [English API documentation](docs/README.en.md)

## License

This project is licensed under the [MIT License](LICENSE).
