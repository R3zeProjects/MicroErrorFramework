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

## Latest full result

The 2026-08-18 Release run completed 91 scenarios. Selected boundary results:

| Scenario | Throughput | p99 producer latency |
| --- | ---: | ---: |
| Parallel logger, 128 B, 1 producer | 8.99M records/s | 300 ns |
| Parallel logger, 128 B, 16 producers | 20.77M records/s | 1.1 us |
| Parallel logger, 8 KiB, 16 producers | 11.67M records/s | 1.5 us |
| Serialized full-metadata logger, 16 producers | 3.31M records/s | 35.3 us |
| Buffered stream sink, 8 producers | 17.84M records/s | 1.0 us |
| Async logger, 1 producer | 1.89M records/s | 3.2 us |
| Async logger, 16 producers | 1.78M records/s | 114.5 us |
| Async logger with 5 us sink, 16 producers | 147,291 records/s | 132.7 us |
| Worker dispatch, q1024, 2 workers, 1 producer | 3.50M tasks/s | 11.1 us |
| Worker backpressure, 20 us tasks | 159,848 tasks/s | 619.5 us |
| Worker tracked futures | 580,472 tasks/s | not sampled |
| Worker native bulk | 4.92M tasks/s | not sampled |
| Same-category register add, 1 producer | 2.19M operations/s | 1.1 us |
| Same-category register add, 16 producers | 2.09M operations/s | 170.9 us |
| Three partitioned register categories | 4.58M operations/s | 1.6 us |

Long 1,024-byte register messages used 300,000 observed normal allocations and
213,600,000 allocated bytes for 100,000 inserts: 3 allocations and 2,136 bytes
per stored error. The probe counts global `new`/`new[]` calls in the benchmark
process; aligned and allocator-internal allocations can differ by platform.

Raw data:

- [`production-full-2026-08-18.csv`](../benchmark-results/production-full-2026-08-18.csv)
- [`production-soak-2026-08-18.csv`](../benchmark-results/production-soak-2026-08-18.csv)

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
