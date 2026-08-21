# Benchmark report

## Environment

- OS: Microsoft Windows 10 Pro 10.0.19045;
- CPU: AMD Ryzen 7 PRO 1700X Eight-Core Processor;
- physical cores: 8;
- logical processors: 16;
- RAM: 31.95 GiB;
- compiler: Clang 22.1.6;
- build mode: Release;
- workload: 100,000 unique inserts;
- date: 2026-08-18.

## Result

### 0.3.0 WorkerPool comparison

The 2026-08-21 comparison ran the same production benchmark source against the
`legacy/0.2.5-beta` branch and the `0.3.0-beta` candidate. Each revision ran five
independent Release samples with 200,000 operations per scenario. Reported
values are medians; the aggregate is the geometric mean of candidate/baseline
throughput ratios for 20 scalar dispatch scenarios.

| Metric | `0.2.5-beta` | `0.3.0-beta` | Change |
| --- | ---: | ---: | ---: |
| Scalar dispatch, q64, 4 producers / 4 workers | 1.45492M/s | 2.86402M/s | **+96.9%** |
| Scalar dispatch, q1024, 4 producers / 4 workers | 1.84961M/s | 2.83218M/s | **+53.1%** |
| Tracked futures, 4 workers | 641,601/s | 1.74017M/s | **+171.2%** |
| Native bulk, 4 workers | 2.95890M/s | 9.62330M/s | **+225.2%** |
| 20-scenario scalar dispatch geometric mean | — | — | **+93.1%** |

The q1024 one-producer/two-worker case measured −7.4%; a 20-microsecond
backpressure workload measured −10.0%, while its task service time dominates
executor overhead. CPU-bound 4,096-iteration work remained within −0.4%.
These regressions are recorded rather than hidden by the aggregate.

The full generated report is
[`v0.2.5-v0.3.0-worker-comparison-2026-08-21.md`](../benchmark-results/v0.2.5-v0.3.0-worker-comparison-2026-08-21.md).
`benchmarks/compare_versions.py` reproduces median and geometric-mean
calculation. The manual Performance regression Action rebuilds both branches,
runs five alternating samples, uploads raw CSV evidence and requires at least
70% scalar-dispatch gain.

One warm-up process was excluded, followed by seven independent Release
processes. The table reports medians:

```text
single operations=100000 median_operations_per_second=4.62022e+06
multi workers=3 operations=99999 median_operations_per_second=5.89096e+06
async operations=1000 median_operations_per_second=923361
worker_pool workers=4 tasks=100000 median_tasks_per_second=514838
worker_dispatch workers=4 tasks=100000 median_tasks_per_second=3.01123e+06
worker_bulk_dispatch workers=4 tasks=100000 median_tasks_per_second=3.44851e+06
```

The multi-threaded run uses three independent categories, so each worker targets
a different register. Contention on a single category is intentionally not
hidden: operations targeting the same register are serialized for correctness.

The async measurement uses `IndustrialWorkerPool` with three bounded workers.
It avoids creating one operating-system thread per submitted task.

### Worker pool modes

A dedicated workload submits 100,000 trivial `OperationResult` tasks to four
workers through a queue with 1,024 slots. Values are medians of seven Release
launches on 2026-08-18:

| Feature | Throughput |
| --- | ---: |
| Tracked `submit()`, preallocated ring queue | **514,838 tasks/s** |
| Fire-and-forget `dispatch()`, preallocated ring queue | **3.01123M tasks/s** |
| Grouped `dispatch_bulk()`, preallocated ring queue | **3.44851M tasks/s** |

Tracked submission returns a future and typed result. Fire-and-forget dispatch
avoids promise/future shared state. The bulk timed region includes creation of
the callback vector, grouped submission, bounded backpressure, execution,
draining, and worker joins. Task bodies intentionally do no application work,
making executor overhead the dominant cost. Tracked, scalar dispatch, and bulk
dispatch are separate API contracts.

The worker-pool measurements cover three distinct public features: tracked
tasks returning typed results, fire-and-forget dispatch, and grouped bulk
submission. The configured run uses four workers, a 1,024-task bounded queue,
blocking backpressure, and 100,000 atomic-increment tasks.

Run the native benchmark with:

```text
cmake -S MicroErrorSystem -B build -DBUILD_BENCHMARKS=ON
cmake --build build --parallel
build/MicroErrorSystemBenchmark
```

On Windows the executable is named `MicroErrorSystemBenchmark.exe`.

## External repository workload

The integration target uses the official MIT-licensed `nlohmann/json` repository,
pinned to tag `v3.12.0` (commit `65ee68451d8eb2b5f3a30b410476ab83deb3289b`).
The workload contains 20,000 JSON documents, including 6,667 malformed inputs:

```text
parse_only documents=20000 median_operations_per_second=108873
parse_and_error_control workers=4 documents=20000 median_operations_per_second=311158
```

This validates the error-control contour around a real third-party parser. It is
not a parser benchmark: the measured result includes parsing, classification,
registration and synchronization according to the selected test case.

## Verification matrix

- Release + CTest: 4/4 native unit/stress/contract tests and 1/1 production benchmark smoke passed;
- Release + external integration + benchmark: 4/4 CTest tests passed;
- local `fmt` + `cpp-httplib` HTTP integration: 4/4 CTest tests passed;
- Clang AddressSanitizer + UndefinedBehaviorSanitizer: 4/4 native tests and
  1/1 production benchmark smoke passed;
- extended Clang warnings and C++23 syntax checks completed without errors.

## Logger and sink throughput

The logger benchmark covers owned-record dispatch, formatting, shared sinks,
sharded sinks, and buffered stream output. Each logger row processes 3,000,000
records. Values are medians from seven Release launches after one excluded
warm-up process. Multi-worker timers start after every worker reaches a latch.

| Logger feature | Workload | Throughput |
| --- | --- | ---: |
| Prepared dispatch | 1 thread | 20.6567M/s |
| Shared-sink dispatch | 4 workers | 17.6403M/s |
| Sharded dispatch | 4 workers | **71.1305M/s** |
| One sink per worker | 4 workers | 68.4916M/s |
| Owned-record formatting | 1 thread | 5.75945M/s |
| Owned-record formatting | 4 workers | 10.3093M/s |

The sharded path uses one thread-owned padded counter per producer and merges
the counters after all workers join. It measures sink topology without changing
the logger's owned-record contract.

The benchmark also isolates the stream sinks with a stream buffer that accepts
all bytes without terminal or filesystem I/O. Building a typical record in a
512-byte stack buffer and issuing one stream write reached 17.0054M records/s.
With four workers writing one shared stream, the immediate sink reached
9.4665M/s. `BufferedStreamSink` reached 17.5995M/s with one producer and
51.6983M/s with four producers, including the final `flush()`. Its thread-local
buffers reduce output-lock acquisitions while preserving per-producer order.
Formatting happens before the stream mutex is acquired. Long messages use the
same owned thread buffer without a temporary formatted record.

| Stream sink workload | Throughput |
| --- | ---: |
| Immediate `ConsoleSink`, 1 thread | 17.0054M/s |
| Immediate shared `ConsoleSink`, 4 workers | 9.4665M/s |
| `BufferedStreamSink`, 1 thread | 17.5995M/s |
| `BufferedStreamSink`, 4 workers | **51.6983M/s** |

The four-worker results have higher scheduler variance than the single-thread
results. The benchmark validates record counts and equal formatted byte counts
before reporting success. Results remain machine- and workload-dependent.

## Fuzzing

- Ubuntu CI runs the coverage-guided LibFuzzer target for 10,000 inputs;
- the Windows Clang run completed the deterministic sanitizer fuzz smoke target
  for 100,000 generated inputs;
- the Windows LibFuzzer link was not used because the installed runtime and
  compiler disagree on the `annotate_string` linker ABI. The portable
  LibFuzzer path remains covered by the Ubuntu job.

## CI quality gates

GitHub Actions additionally defines reproducible jobs for:

- GCC source coverage with an uploaded `coverage.xml` artifact;
- Clang LibFuzzer smoke testing;
- Valgrind Memcheck and Helgrind, including the worker pool;
- external benchmark execution with an uploaded raw result;
- Callgrind profiling with an uploaded `callgrind.out` artifact;
- Windows and Ubuntu build/test matrix.

Coverage is intentionally a diagnostic signal, not a sole quality target:
concurrency correctness is checked separately by ThreadSanitizer, stress tests
and the lifecycle-focused unit tests.

The WorkerPool uses C++ atomic/futex synchronization. Helgrind's own manual
documents that custom atomic/futex primitives require `ANNOTATE_*` client
requests; the Valgrind job therefore enables `ENABLE_HELGRIND_ANNOTATIONS`.
Those annotations describe the same happens-before edges used by the C++ memory
model and are disabled in normal builds. The dedicated Helgrind workload avoids
`std::future` because Helgrind does not model libstdc++ future shared-state
synchronization. It still exercises concurrent producers, bounded backpressure,
scalar and bulk dispatch, waiting, and draining. ThreadSanitizer independently
checks the complete pool, including futures, cancellation, and shutdown. See the
[Helgrind manual](https://valgrind.org/docs/manual/hg-manual.html).
