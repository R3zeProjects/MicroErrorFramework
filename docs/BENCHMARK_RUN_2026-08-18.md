# Full verification and comparison run — 2026-08-18

## Scope and environment

This report records a local production-oriented verification of commit
`5a9cd59d640f`. It covers correctness, sanitizer execution, deterministic fuzzing,
third-party integration, a 91-scenario load matrix, a 90-second soak, and
seven-run comparisons with pinned alternatives.

- Windows 10 Pro 10.0.19045;
- AMD Ryzen 7 PRO 1700X, 8 cores / 16 logical processors, 3.4 GHz nominal;
- 31.95 GiB RAM;
- Clang 22.1.8, `x86_64-pc-windows-msvc`, C++23;
- Release for performance; RelWithDebInfo for Windows sanitizer fuzz smoke.

Pinned comparison sources:

| Library | Version | Commit |
| --- | --- | --- |
| spdlog | 1.15.3 | `6fa36017cfd5` |
| BS::thread_pool | 5.1.0 | `bd4533f1f70c` |
| Taskflow | 4.1.0 | `45366fe5bc4f` |
| fmt | 11.2.0 | `40626af88bd7` |
| nlohmann/json | 3.12.0 | `55f93686c015` |
| cpp-httplib | 0.18.0 | `e64379c3d71c` |

## Verification result

| Gate | Result | Scope |
| --- | --- | --- |
| Windows Clang Release CTest | 5/5 passed | core, logger, worker stress, concurrency contracts, production smoke |
| Windows MSVC 19.51 Release CTest | 5/5 passed | same native matrix with `/W4 /permissive-` |
| Clang ASan + UBSan | 5/5 passed | same native suite under sanitizers |
| Local third-party integration | 5/5 passed | fmt + cpp-httplib plus native suite |
| nlohmann/json stress | 6/6 passed | external parser contour plus native suite |
| Deterministic fuzz smoke | 100,000 inputs passed | Error equality/register lifecycle under ASan + UBSan |
| Production full matrix | 100/100 scenarios passed | counts and failure accounting validated |
| Production soak | 90 seconds passed | 30 seconds per subsystem, zero failures |

The Windows fuzz executable requires the Clang ASan runtime directory in
`PATH`. A Debug build mixes Microsoft Debug CRT with dynamic Clang ASan and can
report a runtime-level invalid free during CRT shutdown. RelWithDebInfo uses a
compatible CRT and completed all 100,000 inputs. Linux CI remains the source of
truth for coverage-guided LibFuzzer, ThreadSanitizer, Valgrind Memcheck and
Helgrind; those tools were not available in the local Windows environment.

## Comparison method

Each comparison executable received one excluded warm-up launch followed by
seven sequential measured launches. The tables report median throughput; the
range exposes scheduler and frequency variance. No benchmark processes ran in
parallel. The compared operations and validation counters are compiled into the
same executable where possible.

### Logger versus spdlog

All rows process 3,000,000 records through null/counting sinks, so terminal and
filesystem latency is excluded.

| Comparable workload | VOSP median/s | spdlog median/s | VOSP / alternative | Observed range, VOSP | Observed range, alternative |
| --- | ---: | ---: | ---: | ---: | ---: |
| Prepared dispatch, 1 thread | 16.088M | 14.227M | **1.131x** | 14.610–16.791M | 13.214–14.886M |
| Shared dispatch, 4 threads | 19.130M | 19.989M | 0.957x | 17.370–22.204M | 16.448–25.977M |
| Equal-output format, 1 thread | 4.784M | 4.805M | 0.996x | 4.312–4.889M | 4.313–4.848M |
| Equal-output format, 4 threads | 9.928M | 9.573M | **1.037x** | 9.451–10.059M | 9.211–12.686M |

The shared four-thread spdlog result has 15.4% coefficient of variation, so its
4.3% lead is not strong enough to claim a stable winner on this host. The
single-thread dispatch advantage is larger than the observed run spread.

VOSP's sharded sink reached 60.741M/s and partitioned sink topology reached
58.966M/s. These are valid VOSP scalability modes but are not direct
replacements for one shared spdlog sink, so they are not used in the A/B ratio.
`BufferedStreamSink` reached 13.008M/s on one thread and 45.404M/s on four.

### Worker pools

Each row executes 100,000 atomic-increment tasks using four workers and bounded
submission. Tracked mode includes future/shared-state creation; dispatch is
fire-and-forget; bulk groups submissions.

| API contract | VOSP median/s | BS::thread_pool median/s | Taskflow median/s |
| --- | ---: | ---: | ---: |
| Tracked future | 591,667 | 499,303 | **859,993** |
| Scalar dispatch | **1,928,790** | 1,784,660 | 1,905,520 |
| Native bulk | **3,128,320** | 2,419,610 | not exposed by this harness |

Relative to BS::thread_pool, VOSP measured 1.185x in tracked mode, 1.081x in
scalar dispatch, and 1.293x in bulk. Taskflow was 1.453x faster than VOSP for
tracked futures; VOSP and Taskflow scalar dispatch were effectively tied
(VOSP 1.012x). These numbers describe trivial-task scheduling overhead, not
application throughput for long-running tasks.

### Raw STL baselines

| Workload | VOSP median/s | Raw STL median/s | VOSP / raw STL |
| --- | ---: | ---: | ---: |
| Single-thread register insert | 3.169M | 3.306M `unordered_set` | 0.958x |
| Three partitioned categories | 6.979M | 4.770M mutex-protected sets | **1.463x** |

The single-thread 4.2% cost buys category validation, duplicate/capacity error
results, owned error values, and the public register contract. The partitioned
result shows the benefit of routing independent categories to independent
locks. Raw STL rows are baselines, not feature-equivalent libraries.

## Native and integration throughput

Seven-run medians from the same Release build:

| Workload | Median/s | Minimum/s | Maximum/s |
| --- | ---: | ---: | ---: |
| Register, single thread | 3,153,080 | 2,750,730 | 3,536,820 |
| Three-category system | 4,270,540 | 3,921,380 | 4,868,740 |
| Async system | 592,417 | 475,285 | 885,740 |
| Worker tracked submit | 530,566 | 502,530 | 633,517 |
| Worker scalar dispatch | 3,335,000 | 3,204,310 | 3,947,890 |
| Worker bulk dispatch | 4,039,420 | 3,746,020 | 4,808,620 |
| nlohmann/json parse only | 95,726 | 89,093 | 114,827 |
| Parse + VOSP error control, 4 workers | 275,475 | 264,016 | 296,481 |

The parser rows exercise 20,000 documents, including 6,667 malformed inputs.
They validate integration and error routing; they are not a claim that VOSP
makes JSON parsing itself faster.

## Production matrix boundaries

The expanded full profile executed 100 scenarios over 1/2/4/8/16 producers and workers,
16 B to 8 KiB messages, bounded queues, slow sinks, rejection, cancellation,
exceptions, contention and allocation accounting. Selected boundaries:

| Scenario | Throughput | p99 producer latency | Failures |
| --- | ---: | ---: | ---: |
| Parallel logger, 128 B, 1 producer | 8.689M/s | 500 ns | 0 |
| Parallel logger, 128 B, 16 producers | 33.430M/s | 700 ns | 0 |
| Parallel logger, 8 KiB, 16 producers | 11.513M/s | 1.4 us | 0 |
| Serialized full metadata, 16 producers | 3.313M/s | 48.0 us | 0 |
| Buffered stream, 8 producers | 20.203M/s | 900 ns | 0 |
| Async logger, 16 producers | 1.854M/s | 224.1 us | 0 |
| Async E2E buffered stream, 1 producer | 3.027M/s | 1.9 us | 0 |
| Async E2E buffered stream, 16 producers | 1.598M/s | 371.9 us | 0 |
| Async 5 us sink, 16 producers | 182,833/s | 371.5 us | 40 expected rejections |
| Worker q1024, 2 workers, 1 producer | 4.517M/s | 4.8 us | 0 |
| Worker 20 us backpressure | 166,824/s | 429.3 us | 0 |
| Worker tracked futures | 648,437/s | not sampled | 0 |
| Worker native bulk | 4.511M/s | not sampled | 0 |
| Same-category register, 16 producers | 2.003M/s | 139.8 us | 0 |
| Three partitioned categories | 5.904M/s | 1.4 us | 0 |

The long-message allocation probe observed 300,000 normal allocations and
213,600,000 bytes for 100,000 stored 1,024-byte errors: 3 allocations and
2,136 bytes per insert. The probe is disabled under ASan so it cannot replace
the sanitizer allocator hooks.

The added async E2E path covers producer submission, bounded queueing,
formatting, buffered stream delivery and both logger/sink flushes. Its 100,000
record memory run observed 201,072 allocations and 22,135,000 bytes. The fixed
1,024-slot worker queue reserves 114,688 bytes on this ABI; `Error` and
`LogEntry` have static sizes of 40 and 56 bytes respectively.

## Thirty-second-per-subsystem soak

| Subsystem | Operations | Throughput | Failures |
| --- | ---: | ---: | ---: |
| Parallel logger, 16 producers | 2,534,965,001 | 84.416M/s | 0 |
| Bounded worker dispatch, 4 producers / 8 workers | 79,773,438 | 2.658M/s | 0 |
| Register add/remove, 8 producers | 133,617,894 | 4.453M/s | 0 |

## Raw data and reproduction

- [`comparisons-7run-2026-08-18.csv`](../benchmark-results/comparisons-7run-2026-08-18.csv)
- [`production-full-verification-2026-08-18.csv`](../benchmark-results/production-full-verification-2026-08-18.csv)
- [`production-soak-30s-2026-08-18.csv`](../benchmark-results/production-soak-30s-2026-08-18.csv)
- [`production-full-candidate-2026-08-18.csv`](../benchmark-results/production-full-candidate-2026-08-18.csv)

Build comparison targets with local pinned source trees:

```text
cmake -S . -B build-compare -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_LOCAL_LIBRARY_COMPARISONS=ON \
  -DLOCAL_EXTERNAL_WORKLOAD_ROOT=../third_party/workloads
cmake --build build-compare --parallel
build-compare/MicroErrorSystemSpdlogComparisonBenchmark
build-compare/MicroErrorSystemWorkerPoolComparisonBenchmark
```

Run production profiles:

```text
build/MicroErrorSystemProductionBenchmark --profile=full --csv=production-full.csv
build/MicroErrorSystemProductionBenchmark --profile=soak --duration=30 --csv=production-soak.csv
```

Results are machine-, compiler-, topology-, power-policy- and workload-specific.
Null sinks isolate framework overhead; production capacity must be retested with
the target filesystem/network sink and realistic message/service-time
distribution.
