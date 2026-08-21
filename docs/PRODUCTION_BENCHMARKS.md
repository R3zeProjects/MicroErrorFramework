# Production benchmark suite

## Purpose

`MicroErrorSystemProductionBenchmark` measures complete public API paths under
burst, contention, saturation, failure, memory, and sustained-load scenarios.
It complements the small historical microbenchmarks; it does not replace an
application-specific capacity test.

The executable emits CSV with throughput, accepted/failed operation counts,
sampled producer-call p50/p95/p99/max latency, and allocation totals where the
portable allocation probe is enabled. Every scenario validates its final record
or task count and exits unsuccessfully on a mismatch.

## Profiles

| Profile | Scope |
| --- | --- |
| `quick` | Reduced 1/4/16-producer matrix for local iteration and CTest |
| `full` | 1/2/4/8/16 producers, 1/2/4/8/16 workers, 16 B–8 KiB messages |
| `soak` | Continuous logger, worker-pool, and register workloads |

Run the complete profile:

```text
cmake -S . -B build-production -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build-production --target MicroErrorSystemProductionBenchmark --parallel
build-production/MicroErrorSystemProductionBenchmark \
  --profile=full --csv=production-full.csv
```

During focused iteration, select one subsystem without changing its scenarios:

```text
build-production/MicroErrorSystemProductionBenchmark \
  --profile=full --suite=worker --operations=200000 --csv=worker.csv
```

Supported suite values are `all`, `logger`, `worker`, and `register`. The
default is `all`; the soak profile intentionally runs every subsystem.

Run a 60-second soak for each subsystem:

```text
build-production/MicroErrorSystemProductionBenchmark \
  --profile=soak --duration=60 --csv=production-soak.csv
```

On Windows append `.exe` to the executable path. Direct sanitizer runs must have
the compiler's ASan runtime directory in `PATH`.

## Covered load classes

### Logger and sinks

- parallel sharded dispatch with 1/2/4/8/16 producers;
- 16, 128, 1,024, and 8,192-byte owned messages;
- serialized dispatch with full timestamp/thread metadata;
- compile-time level filtering;
- 1/2/4/8-sink fan-out;
- immediate and per-thread buffered stream output;
- bounded asynchronous logging, flush, and producer backpressure;
- end-to-end async queue, formatting, buffered stream and flush;
- 5-microsecond slow sink and 1% sink rejection propagation.

### Worker pool

- 1/2/4/8/16 workers;
- 64- and 1,024-slot bounded queues;
- one and four concurrent producers;
- tracked futures, scalar dispatch, and native bulk dispatch;
- 1- and 20-microsecond service time under saturation;
- CPU-bound tasks with 256 and 4,096 mixing iterations;
- result failures, thrown exceptions, queue cancellation, and future completion.

### Registers and routing

- same-category contention with 1/2/4/8/16 producers;
- three independently routed categories;
- duplicate, capacity, and missing-category errors;
- concurrent successful `contains()` and `remove()` paths;
- long-message allocation accounting.
- static `Error`/`LogEntry`, worker queue, and async record allocation metrics.

## Latest full result

The latest version-to-version WorkerPool result is documented in
[BENCHMARKS.md](BENCHMARKS.md) and the generated
[0.2.5-to-0.3.0 report](../benchmark-results/v0.2.5-v0.3.0-worker-comparison-2026-08-21.md).
The table below remains the complete 2026-08-18 production matrix so historical
results are not silently rewritten.

The 2026-08-18 production-candidate Release run completed 100 scenarios.
Selected boundary results:

| Scenario | Throughput | p99 producer latency |
| --- | ---: | ---: |
| Parallel logger, 128 B, 1 producer | 8.69M records/s | 500 ns |
| Parallel logger, 128 B, 16 producers | 33.43M records/s | 700 ns |
| Parallel logger, 8 KiB, 16 producers | 11.51M records/s | 1.4 us |
| Serialized full-metadata logger, 16 producers | 3.31M records/s | 48.0 us |
| Buffered stream sink, 8 producers | 20.20M records/s | 900 ns |
| Async logger, 1 producer | 2.99M records/s | 1.9 us |
| Async logger, 16 producers | 1.85M records/s | 224.1 us |
| Async logger with 5 us sink, 16 producers | 182,833 records/s | 371.5 us |
| Async E2E buffered stream, 1 producer | 3.03M records/s | 1.9 us |
| Async E2E buffered stream, 16 producers | 1.60M records/s | 371.9 us |
| Worker dispatch, q1024, 2 workers, 1 producer | 4.52M tasks/s | 4.8 us |
| Worker backpressure, 20 us tasks | 166,824 tasks/s | 429.3 us |
| Worker tracked futures | 648,437 tasks/s | not sampled |
| Worker native bulk | 4.51M tasks/s | not sampled |
| Same-category register add, 1 producer | 2.39M operations/s | 1.2 us |
| Same-category register add, 16 producers | 2.00M operations/s | 139.8 us |
| Three partitioned register categories | 5.90M operations/s | 1.4 us |

Long 1,024-byte register messages used 300,000 observed normal allocations and
213,600,000 allocated bytes for 100,000 inserts: 3 allocations and 2,136 bytes
per stored error. The probe counts global `new`/`new[]` calls in the benchmark
process; aligned and allocator-internal allocations can differ by platform.

The async 128-byte pipeline observed 201,072 allocations and 22,135,000 bytes
for 100,000 accepted records: approximately 2.01 allocations and 221 bytes per
record. The fixed 1,024-slot worker ring reserved 114,688 bytes. On this ABI,
`sizeof(Error)` is 40 bytes and `sizeof(LogEntry)` is 56 bytes; owned message
storage is additional.

Raw data:

- [`production-full-2026-08-18.csv`](../benchmark-results/production-full-2026-08-18.csv)
- [`production-soak-2026-08-18.csv`](../benchmark-results/production-soak-2026-08-18.csv)
- [`production-full-verification-2026-08-18.csv`](../benchmark-results/production-full-verification-2026-08-18.csv)
- [`production-soak-30s-2026-08-18.csv`](../benchmark-results/production-soak-30s-2026-08-18.csv)
- [`production-full-candidate-2026-08-18.csv`](../benchmark-results/production-full-candidate-2026-08-18.csv)

## Thirty-second soak verification

| Subsystem | Operations | Throughput | Failures |
| --- | ---: | ---: | ---: |
| Parallel logger, 16 producers | 2,534,965,001 | 84.42M/s | 0 |
| Bounded worker dispatch, 4 producers / 8 workers | 79,773,438 | 2.66M/s | 0 |
| Register add/remove, 8 producers | 133,617,894 | 4.45M/s | 0 |

## Ten-second soak result

| Subsystem | Operations | Throughput | Failures |
| --- | ---: | ---: | ---: |
| Parallel logger, 16 producers | 908,467,855 | 90.73M/s | 0 |
| Bounded worker dispatch, 4 producers / 8 workers | 29,734,248 | 2.97M/s | 0 |
| Register add/remove, 8 producers | 45,280,700 | 4.52M/s | 0 |

## Interpretation limits

- Latency sampling uses `steady_clock` on a subset of operations; throughput
  includes that small sampling cost.
- The null stream accepts bytes without terminal, filesystem, or SSD latency.
- Busy-wait service times model CPU-visible slow consumers, not OS sleep or I/O.
- Allocation counts are disabled under AddressSanitizer to avoid replacing its
  allocator hooks.
- Results are machine-, compiler-, power-policy-, and workload-dependent.
- A production deployment must repeat the full and soak profiles on its target
  hardware with its real sinks, messages, and service-time distribution.
