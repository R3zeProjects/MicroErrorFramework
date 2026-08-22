# MicroErrorFramework

**MicroErrorFramework** is a C++23 micro-framework for building a
system-independent error handling and logging contour.

It provides one public API for classifying, registering, routing, logging,
and asynchronously processing errors without coupling the application to a
particular operating system or domain.

> **Main idea:** an error-control and logging contour independent of the host system.

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![API](https://img.shields.io/badge/API-0.4.1--beta-orange)
![CMake](https://img.shields.io/badge/CMake-3.25%2B-064F8C)
![License](https://img.shields.io/badge/license-MIT-green)

## Why this project exists

Application code often mixes domain logic, error storage, logging, thread
management, and shutdown handling. MicroErrorFramework separates these concerns
behind small interfaces and compile-time policies.

The framework is intentionally small and header-only. It is suitable for
experiments, infrastructure components, services, and as a reusable error
control layer in larger C++ systems.

## Features

- strongly typed `Error`, `Result<T>`, and `OperationResult` based on
  `std::expected`;
- category-specific registers with `IRegister` and `CategoryRegister`;
- unified `Register<Category, Policy>` with bounded capacity;
- owning register lookup by code, snapshots, and explicit bulk clearing;
- category routing through `Handler` and `ConcurrentHandler`;
- compile-time single-threaded, multi-threaded, and asynchronous systems;
- `WorkerPool` with bounded workers and bounded queue;
- blocking backpressure when the queue is full;
- cooperative cancellation with `std::stop_token`;
- queue cleanup and configurable drain/cancel shutdown behavior;
- extensible logger with custom sinks and compile-time filtering policies;
- exception-to-`Result` capture with direct failure logging;
- RAII-based ownership and safe asynchronous lifetime handling;
- CTest, stress tests, AddressSanitizer, UndefinedBehaviorSanitizer,
  ThreadSanitizer, LibFuzzer, Valgrind, coverage, and Callgrind workflows;
- external integration workloads using pinned third-party repositories.

## Measured results

All values below are real local Release measurements, not theoretical claims.

| Feature | Measured workload | Result |
| --- | --- | ---: |
| Typed register | 100,000 single-threaded inserts | **4.62022M operations/s** |
| Register subsystem gain | Seven-run geometric mean vs `v0.3.1-beta` | **+57.1%** |
| Full-history microbenchmark gain | Six unchanged scenarios vs `v0.1.0-beta` | **+73.1%** |
| Full-history production gain | 96 common scenarios vs `v0.2.0-beta` | **+34.5%** |
| Concurrent category routing | 99,999 inserts over 3 workers | **5.89096M operations/s** |
| Asynchronous error system | 1,000 completed operations | **923,361 operations/s** |
| Tracked worker tasks | `submit()`, 4 workers | **1.74017M tasks/s** |
| Fire-and-forget tasks | `dispatch()`, q64, 4 producers / 4 workers | **2.86402M tasks/s** |
| Grouped task submission | `dispatch_bulk()`, 4 workers | **9.62330M tasks/s** |
| Worker dispatch gain | 20-scenario geometric mean vs `v0.2.5-beta` | **+93.1%** |
| Worker dispatch patch gain | 20-scenario geometric mean vs `v0.3.0-beta` | **+15.3%** |
| Bulk dispatch gain | Same machine and harness vs `v0.2.5-beta` | **+225.2%** |
| Async queue allocation bytes | 200,000 records vs `v0.2.5-beta` | **−19.4%** |
| Logger dispatch | Prepared records, 1 thread | **20.6567M records/s** |
| Sharded logger dispatch | Prepared records, 4 workers | **71.1305M records/s** |
| Owned-record formatting | Equal-output formatting, 1 thread | **5.75945M records/s** |
| Owned-record formatting | Equal-output formatting, 4 workers | **10.3093M records/s** |
| Immediate stream sink | default `Sink`, 1 thread | **17.0054M records/s** |
| Buffered stream sink | Per-thread buffering, 4 workers | **51.6983M records/s** |
| JSON integration | Parse and route errors, 4 workers | **311,158 documents/s** |
| HTTP integration | Concurrent local workload | **1,000 requests; 500 errors routed** |

Verification results:

```text
Clang Release CTest                  11/11 passed
Production benchmark smoke            1/1 passed
MSVC Release CTest                   11/11 passed
nlohmann/json external CTest          6/6 passed
fmt + cpp-httplib local CTest         5/5 passed
Windows sanitizer fuzz smoke        100,000 inputs passed
```

The benchmark machine was an AMD Ryzen 7 PRO 1700X with 8 physical cores,
16 logical processors, 31.95 GiB RAM, Windows 10 Pro, and Clang 22.1.6.
Detailed methodology and raw measurement ranges are available in
[docs/BENCHMARKS.md](docs/BENCHMARKS.md). Exact median/minimum/maximum values
for the latest full run are in
[docs/BENCHMARK_RUN_2026-08-18.md](docs/BENCHMARK_RUN_2026-08-18.md).
The production matrix, latency percentiles, saturation scenarios, and soak
results are documented in
[docs/PRODUCTION_BENCHMARKS.md](docs/PRODUCTION_BENCHMARKS.md).
The complete ten-release comparison and machine-readable medians are recorded
in [the historical report](benchmark-results/v0.1.0-v0.4.0-history-2026-08-21.md)
and its [CSV dataset](benchmark-results/v0.1.0-v0.4.0-medians-2026-08-21.csv).

Logger values are medians of seven Release launches after one excluded warm-up
process over 3,000,000 owned records. Worker construction is excluded with a
start latch. The logger keeps `Error` and asynchronous `LogEntry` payloads
owning; the benchmark does not weaken those lifetime guarantees. Results are
component measurements, not universal performance guarantees.

The common one-sink path uses an atomic sink pointer and avoids the registry
mutex. `ParallelSinkDispatch` removes callback serialization for thread-safe
sinks, while `MinimalMetadataPolicy` omits timestamp and thread-id collection.
The same `Logger` therefore covers safe, parallel, and latency-oriented
modes without a separate fast-logger class. Throughput remains machine-dependent.

The `0.3.0-beta` comparison uses five independent Release runs per revision,
200,000 operations per worker scenario, and the exact `v0.2.5-beta` source from
the `legacy/0.2.5-beta` branch. The raw medians and scenario deltas are recorded
in [the version comparison](benchmark-results/v0.2.5-v0.3.0-worker-comparison-2026-08-21.md).
The `0.3.1-beta` comparison uses seven alternating Release runs against the
immutable `v0.3.0-beta` tag. It measured a 15.3% dispatch geometric-mean gain;
details and recorded regressions are in
[the patch comparison](benchmark-results/v0.3.0-v0.3.1-worker-comparison-2026-08-21.md).
The `0.4.0-beta` register comparison measured a 57.1% subsystem geometric-mean
gain; see [the feature comparison](benchmark-results/v0.3.1-v0.4.0-register-comparison-2026-08-21.md).
The Performance regression Action runs automatically for relevant main-branch
pushes and pull requests. Feature releases are checked against `v0.3.1-beta`
with bounded scenario and subsystem regression gates.

## Architecture

```text
Error ──► System ──► Handler ──► Register ──► OperationResult
  │
  └────► Logger ──► Sink

System<system_policy::Async<Executor>> ──► external executor
```

The system mode is selected by types rather than runtime flags:

```cpp
using Single = vosp::error::System<
    vosp::error::system_policy::SingleThreaded, RegisterA, RegisterB>;
using Multi = vosp::error::System<
    vosp::error::system_policy::MultiThreaded, RegisterA, RegisterB>;
using Async = vosp::error::System<
    vosp::error::system_policy::Async<Executor>, RegisterA, RegisterB>;
```

## Why policies are needed

Policies keep execution and logging decisions explicit at compile time:

- `register_policy` selects internal register synchronization;
- `system_policy` selects direct, synchronized, or asynchronous routing;
- `logger_policy` selects filtering, dispatch, and metadata collection;
- `logger_policy::Minimum<Level::WARNING>` removes lower-severity records before
  sink callbacks, reducing work and output volume.

This avoids runtime mode switches in hot paths and lets the compiler reject an
invalid policy type. Policies are intentionally small stateless types, so they
do not add per-object configuration storage. They should describe behavior,
not own resources; ownership remains explicit in registers, sinks, executors,
and worker pools.

Registers are non-owning dependencies and must outlive the objects that use
them. Logger sinks may be non-owning references or logger-owned
`std::shared_ptr` dependencies. The built-in asynchronous system keeps its
handler state alive for submitted operations so destroying the system does not
invalidate pending futures.

## Requirements

- CMake 3.25 or newer;
- a compiler with C++23 support;
- a standard library implementation providing `std::expected`;
- Git for optional external integration workloads.

GitHub Actions now defines Windows MSVC/Clang and Linux GCC/Clang gates. These
jobs become verified evidence only after a green remote run. The LibFuzzer,
ThreadSanitizer, Valgrind and nightly soak configurations target Linux.

## Quick start

From the repository root:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows, the executable suffix is `.exe`.

Install the header-only package and consume it through `find_package`:

```bash
cmake --install build --prefix install
cmake -S consumer -B consumer/build \
  -DCMAKE_PREFIX_PATH=/path/to/install
```

```cmake
find_package(vosp 0.4.1 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE vosp::vosp)
```

## Minimal example

```cpp
#include <vosp.hpp>
#include <iostream>
#include <stdexcept>

using namespace vosp::error;

Register<Category::NETWORK> network;
System<system_policy::SingleThreaded, decltype(network)> system{network};

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
    const OperationResult removed = system.remove(error);
}

const auto stored = network.find(1001);
const auto current_errors = network.snapshot();
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

At exception boundaries, preserve the same `Result<T>` channel and optionally
log the converted failure:

```cpp
vosp::logger::Sink sink{std::cerr};
vsp::Logger logger{sink};

Result<int> rows = logger.capture(
    Error{Category::DATABASE, 2001, "database query"},
    []() -> int {
        throw std::runtime_error{"timeout"};
    });

// rows.error(): [DATABASE:2001] database query: timeout
```

## Worker pool

```cpp
#include <vosp.hpp>

vosp::async::WorkerPool pool{
    4,    // worker count
    128   // queue capacity
};

auto future = pool.submit([] {
    return vosp::error::OperationResult{};
});

pool.dispatch([] {
    return vosp::error::OperationResult{};
});

const vosp::error::OperationResult result = future.get();
pool.shutdown(vosp::async::ShutdownMode::DRAIN);
```

The hard limits are 1,024 workers and 1,024 queued tasks. `submit()` applies
blocking backpressure. Queue slots are preallocated and reused without
per-task queue-node allocation. `clear_queue()` completes pending futures with
a cancellation error. `ShutdownMode::DRAIN` executes accepted work without
requesting cancellation; `CANCEL_PENDING` cancels queued work and requests
cooperative cancellation from active tasks. A running task is not forcefully
interrupted, so cancellable tasks must inspect their `std::stop_token`.
Use `submit()` when the caller needs a future. `dispatch()` avoids the
promise/future allocation for fire-and-forget work; failures and cancelled
dispatches remain observable through `failed_dispatches()` and
`cancelled_dispatches()`.
For sustained ingestion, `dispatch_bulk(std::span<Task>)` moves callbacks into
the ring queue using grouped refills. It preserves bounded backpressure and
returns the exact accepted count if shutdown interrupts a batch.

## Logging

Choose sink behavior through `sink_policy`. The default `Sink` writes each
record immediately and is the clearest option for diagnostics:

```cpp
#include <vosp.hpp>
#include <iostream>

vosp::logger::Sink output{std::cout};
vsp::Logger logger{output};

const bool logged = logger.error(vosp::error::Error{
    vosp::error::Category::NETWORK,
    1001,
    "Connection refused"
});
```

For concurrent producers, select `sink_policy::Buffered` and parallel logger
dispatch without changing the logging calls:

```cpp
#include <vosp.hpp>
#include <iostream>

vosp::logger::Sink<vosp::logger::sink_policy::Buffered> sink{std::cout};
vosp::logger::Logger<
    vosp::logger::logger_policy::AcceptAll,
    vosp::logger::logger_policy::Parallel> logger{sink};

const bool logged = logger.info(vosp::error::Error{
    vosp::error::Category::NETWORK,
    1002,
    "Request completed"
});

if (!sink.flush()) {
    // Handle destination stream failure.
}
```

The default per-thread flush threshold is 64 KiB and may be supplied as the
second constructor argument. Call `flush()` after producer threads have joined
and before destroying the destination stream. Order is preserved within each
producer thread; global order between producers is unspecified.

Custom sinks implement `ILogSink`. `Logger` supports compile-time filtering,
for example `logger_policy::Minimum<Level::WARNING>`. A reference-based
sink has external ownership and must outlive the logger. To make the logger
retain a sink safely, attach a `std::shared_ptr` instead:

```cpp
#include <memory>

auto sink = std::make_shared<MySink>();
vsp::Logger logger{sink};
sink.reset(); // Logger keeps the sink alive.
```

The default `Logger` serializes callbacks, so it is suitable for ordinary sinks
that do not provide synchronization. Select `logger_policy::Parallel` only
when every attached sink safely supports concurrent calls to `ILogSink::write`:

```cpp
#include <atomic>

class AtomicSink final : public vosp::logger::ILogSink
{
public:
    [[nodiscard]] bool write(const vosp::logger::LogEntry&) override
    {
        records_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

private:
    std::atomic_uint32_t records_ = 0;
};

AtomicSink sink;
vosp::logger::Logger<
    vosp::logger::logger_policy::AcceptAll,
    vosp::logger::logger_policy::Parallel> logger{sink};
```

For the lowest overhead, compose the same logger with parallel dispatch and
minimal metadata:

```cpp
AtomicSink sink;
vosp::logger::Logger<
    vosp::logger::logger_policy::AcceptAll,
    vosp::logger::logger_policy::Parallel,
    vosp::logger::logger_policy::MinimalMetadata> logger{sink};

const bool logged = logger.info(vosp::error::Error{
    vosp::error::Category::NETWORK, 1001, "Connection refused"});
```

The asynchronous mode uses the same API and owns queued `Error` values. Its
1,024-record bounded queue applies blocking backpressure and drains records in
batches:

```cpp
vosp::logger::Logger<
    vosp::logger::logger_policy::AcceptAll,
    vosp::logger::logger_policy::Async,
    vosp::logger::logger_policy::MinimalMetadata> async_logger{sink};

const bool queued = async_logger.info(vosp::error::Error{
    vosp::error::Category::NETWORK, 1002, "Queued safely"});
async_logger.flush();
```

## Build profiles

### Benchmarks

```bash
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
build-bench/MicroErrorSystemBenchmark
```

### AddressSanitizer and UndefinedBehaviorSanitizer

```bash
cmake -G Ninja -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

### ThreadSanitizer

```bash
cmake -G Ninja -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DENABLE_THREAD_SANITIZER=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

### Coverage

```bash
cmake -S . -B build-coverage \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DENABLE_COVERAGE=ON
cmake --build build-coverage --parallel
ctest --test-dir build-coverage --output-on-failure
gcovr --root . \
  --filter 'include/.*' \
  --txt --xml-pretty --output coverage.xml
```

Coverage is a diagnostic signal. Thread-safety and lifecycle correctness are
also checked with stress tests, ThreadSanitizer, and targeted unit tests.

### LibFuzzer

```bash
cmake -G Ninja -S . -B build-fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_FUZZERS=ON
cmake --build build-fuzz --parallel
build-fuzz/MicroErrorSystemFuzzer -runs=10000
```

For Windows or toolchains without a compatible LibFuzzer runtime, the
repository also provides a sanitizer-backed deterministic smoke target:

```bash
cmake -G Ninja -S . -B build-fuzz-smoke \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_FUZZ_SMOKE=ON \
  -DBUILD_TESTING=OFF
cmake --build build-fuzz-smoke --parallel
build-fuzz-smoke/MicroErrorSystemFuzzSmoke
```

The smoke target executes 100,000 generated inputs through the same fuzz
harness under AddressSanitizer and UndefinedBehaviorSanitizer. It complements
coverage-guided LibFuzzer in CI. On Windows, use `RelWithDebInfo`: the dynamic
Clang ASan runtime is not compatible with the Microsoft Debug CRT used by a
`Debug` build. Add the compiler's ASan runtime directory to `PATH` before
launching the executable.

## Real benchmark results

The following results were collected from a local Release build on:

- Windows 10 Pro;
- AMD Ryzen 7 PRO 1700X, 8 physical cores / 16 logical processors;
- 31.95 GiB RAM;
- Clang 22.1.8;
- benchmark date: 2026-08-18.

Native API workload:

```text
single operations=100000 median_operations_per_second=4.62022e+06
multi workers=3 operations=99999 median_operations_per_second=5.89096e+06
async operations=1000 median_operations_per_second=923361
worker_pool workers=4 tasks=100000 median_tasks_per_second=514838
worker_dispatch workers=4 tasks=100000 median_tasks_per_second=3.01123e+06
worker_bulk_dispatch workers=4 tasks=100000 median_tasks_per_second=3.44851e+06
```

External integration workload using the official `nlohmann/json` repository,
pinned to `v3.12.0`:

```text
external_repo=nlohmann/json@v3.12.0 documents=20000 malformed=6667
parse_only documents=20000 median_operations_per_second=108873
parse_and_error_control workers=4 documents=20000 median_operations_per_second=311158
```

These are reproducible local measurements, not performance guarantees. Results
depend on CPU frequency policy, compiler, build mode, dependency version,
workload composition, and background processes. Full methodology is available
in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

### External HTTP integration workload

The local integration combines `fmt 11.2.0`, `cpp-httplib v0.18.0`, and
MicroErrorFramework's multi-threaded `System`. Four workers issue requests against a
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
cmake -S . -B build-external \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARKS=ON \
  -DBUILD_EXTERNAL_STRESS_TESTS=ON
cmake --build build-external --parallel
ctest --test-dir build-external --output-on-failure
build-external/MicroErrorSystemExternalBenchmark
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
| [`gabime/spdlog`](https://github.com/gabime/spdlog) | `v1.15.3` | optional logger workload validation |
| [`bshoshany/thread-pool`](https://github.com/bshoshany/thread-pool) | `v5.1.0` | optional worker-pool workload validation |
| [`taskflow/taskflow`](https://github.com/taskflow/taskflow) | `v4.1.0` | optional executor workload validation |

The `nlohmann/json` workload is wired into the external CMake stress target.
The `fmt` and `cpp-httplib` repositories are wired into an opt-in local HTTP
integration target. BS::thread_pool and Taskflow are used only by the local
worker-pool comparison target. Their source trees are ignored by Git and are
not shipped as runtime dependencies.

## Verification status

The project currently includes:

- current Clang and MSVC Release unit/stress/contract suites: `11/11` passed;
- Release production benchmark smoke: `1/1` passed;
- external integration tests: `4/4` passed;
- local `fmt` + `cpp-httplib` integration: `4/4` passed;
- Clang ASan/UBSan unit/stress/contract tests: `4/4` passed;
- Clang ASan/UBSan production benchmark smoke: `1/1` passed;
- extended C++23 warning and syntax checks without errors;
- LibFuzzer smoke test configuration;
- Valgrind Memcheck and Helgrind CI jobs;
- GCC coverage and Callgrind CI artifacts;
- Windows and Ubuntu CI build/test matrix.

## Repository layout

```text
MicroErrorFramework/
├── include/
│   ├── vosp.hpp                 # single public entry point
│   ├── vosp_error.hpp           # Error, Result, registers and systems
│   ├── vosp_logger.hpp          # logger, policies and sinks
│   └── vosp_worker_pool.hpp     # bounded asynchronous worker pool
├── tests/                       # unit, concurrency and external stress tests
├── benchmarks/                  # native and third-party benchmarks
├── fuzz/                        # LibFuzzer target
├── docs/
│   ├── ARCHITECTURE.md          # components, ownership and control flow
│   ├── API_CONTRACTS.md         # lifetime and concurrency guarantees
│   ├── README.en.md             # focused API guide
│   └── BENCHMARKS.md            # benchmark methodology and results
├── CMakeLists.txt
└── .github/workflows/ci.yml
```

## Versioning

The current release is **`0.4.1-beta`**. The numeric API version follows this
project policy:

- `x.0.0` — a complete system redesign or stable generation release;
- `0.x.0` — a major feature update while the API is still pre-1.0;
- `0.0.x` — a backward-compatible patch or bug fix.

The `-beta` suffix marks a prerelease whose API can still change before the
first stable generation.

## Current limitations

- The API is version `0.4.1-beta` and may evolve before `1.0.0`.
- Error systems reference externally owned registers; logger sinks are either
  non-owning references or logger-owned `std::shared_ptr` instances.
- A running worker task cannot be forcefully interrupted safely.
- `Category::NONE` is not routed to a specialized category register.
- `Error` stores its message in `std::string`, so predefined errors are
  `inline const` values rather than `inline constexpr` objects.
- Benchmark values are machine-dependent and should be compared only under the
  same toolchain and workload.
- Hosted nightly CI runs a three-hour soak by default. A 12-hour release soak
  requires a self-hosted runner or an external orchestration environment.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Public API and concurrency contracts](docs/API_CONTRACTS.md)
- [Unified API migration](docs/API_MIGRATION.md)
- [Benchmark report](docs/BENCHMARKS.md)
- [External workload validation](docs/EXTERNAL_WORKLOADS.md)
- [API guide](docs/README.en.md)
- [Changelog](CHANGELOG.md)

## License

This project is licensed under the [MIT License](LICENSE).
