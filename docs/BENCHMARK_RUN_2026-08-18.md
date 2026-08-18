# Full benchmark run — 2026-08-18

## Environment

- OS: Microsoft Windows 10 Pro 10.0.19045;
- CPU: AMD Ryzen 7 PRO 1700X, 8 cores / 16 logical processors, 3.4 GHz nominal;
- RAM: 31.95 GiB;
- compiler: Clang 22.1.6, `x86_64-w64-windows-gnu`;
- language mode: C++23;
- build mode: Release;
- integration dependency: nlohmann/json 3.12.0.

## Method

Each executable received one excluded warm-up process followed by seven
independent measured process launches. Executables ran sequentially so benchmark
groups did not compete for CPU time. The median is the reported result; minimum
and maximum show observed scheduler and frequency variance. Every target checks
its operation counts before returning success.

## Core API

| Workload | Median/s | Minimum/s | Maximum/s |
| --- | ---: | ---: | ---: |
| Register, single thread, 100,000 inserts | 4,620,220 | 3,430,770 | 5,157,300 |
| Error system, 3 workers, 99,999 inserts | 5,890,960 | 4,441,240 | 8,596,150 |
| Async system, 1,000 operations | 923,361 | 759,301 | 1,216,550 |
| Worker tracked submit, 100,000 tasks | 514,838 | 472,373 | 565,566 |
| Worker scalar dispatch, 100,000 tasks | 3,011,230 | 2,493,390 | 3,161,760 |
| Worker bulk dispatch, 100,000 tasks | 3,448,510 | 3,054,550 | 3,695,220 |

## Logger and stream sinks

All logger rows process 3,000,000 owned records.

| Workload | Median/s | Minimum/s | Maximum/s |
| --- | ---: | ---: | ---: |
| VOSP prepared dispatch, 1 thread | 20,656,700 | 17,026,400 | 21,476,400 |
| VOSP prepared dispatch, shared sink, 4 workers | 17,640,300 | 17,183,900 | 25,856,500 |
| VOSP sharded dispatch, 4 workers | 71,130,500 | 65,563,700 | 76,066,800 |
| VOSP one sink per worker | 68,491,600 | 57,937,400 | 70,200,100 |
| VOSP equal-output formatting, 1 thread | 5,759,450 | 5,340,360 | 6,026,730 |
| VOSP equal-output formatting, 4 workers | 10,309,300 | 9,791,510 | 11,027,700 |
| Immediate `ConsoleSink`, 1 thread | 17,005,400 | 12,000,300 | 19,134,100 |
| Immediate `ConsoleSink`, 4 workers | 9,466,500 | 9,058,870 | 10,542,000 |
| `BufferedStreamSink`, 1 thread | 17,599,500 | 13,137,600 | 20,038,700 |
| `BufferedStreamSink`, 4 workers | 51,698,300 | 48,572,800 | 56,050,700 |

VOSP owns the message stored in `Error` and in asynchronous `LogEntry` values.
Sharded dispatch and per-thread stream buffers reduce contention while keeping
that lifetime contract.

## Worker-pool features

Each row executes 100,000 atomic-increment tasks on four workers.

| Workload | Median/s | Minimum/s | Maximum/s |
| --- | ---: | ---: | ---: |
| VOSP tracked future | 552,526 | 525,574 | 635,465 |
| VOSP scalar dispatch | 1,622,300 | 1,445,360 | 2,006,740 |
| VOSP native bulk | 2,821,910 | 2,603,960 | 3,040,990 |

Every row uses the configured 1,024-slot bounded queue and blocking
backpressure.

## nlohmann/json integration

The workload processes 20,000 documents, including 6,667 malformed inputs.

| Workload | Median/s | Minimum/s | Maximum/s |
| --- | ---: | ---: | ---: |
| Parse + VOSP error control, 4 workers | 311,158 | 283,962 | 324,628 |

The integration classifies malformed inputs, routes errors, and stores them in
the register system.

## Reproduction

Build the targets in Release mode and run each executable once for warm-up and
seven more times for measurement:

```text
MicroErrorSystemBenchmark
MicroErrorSystemComparisonBenchmark
MicroErrorSystemSpdlogComparisonBenchmark
MicroErrorSystemWorkerPoolComparisonBenchmark
MicroErrorSystemExternalBenchmark
```

The numbers are local measurements, not universal performance guarantees.
