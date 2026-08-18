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
- date: 2026-08-17.

## Result

One local Release run produced the following native results:

```text
single operations=100000 elapsed_us=27530 operations_per_second=3.6324e+06
multi workers=3 operations=99999 elapsed_us=22059 operations_per_second=4.53325e+06
async operations=1000 elapsed_us=4087 operations_per_second=244678
```

The multi-threaded run uses three independent categories, so each worker targets
a different register. Contention on a single category is intentionally not
hidden: operations targeting the same register are serialized for correctness.

The async measurement uses `IndustrialWorkerPool` with three bounded workers.
It avoids creating one operating-system thread per submitted task.

This is an indicative local measurement, not a stable performance guarantee.
For comparable results, use the same compiler, build type, CPU frequency policy,
and workload. Run the benchmark with:

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
parse_only elapsed_us=178771 operations_per_second=111875
parse_and_error_control workers=4 elapsed_us=89050 operations_per_second=224593
```

This validates the error-control contour around a real third-party parser. It is
not a parser benchmark: the measured result includes parsing, classification,
registration and synchronization according to the selected test case.

## Verification matrix

- Release + CTest: 3/3 native tests passed;
- Release + external integration + benchmark: 4/4 CTest tests passed;
- local `fmt` + `cpp-httplib` HTTP integration: 4/4 CTest tests passed;
- Clang AddressSanitizer + UndefinedBehaviorSanitizer: 3/3 native tests passed;
- extended Clang warnings and C++23 syntax checks completed without errors.

## Baseline comparison

The comparison target measures the framework against raw standard-library
containers under the same 100,000-unique-insert workload:

```text
raw_unordered_set operations_per_second=4.85437e+06
memory_register operations=100000 operations_per_second=4.97711e+06
raw_mutex_sets operations_per_second=8.09512e+06
memory_register_parallel operations_per_second=1.30309e+07
```

The raw baseline intentionally omits category validation, capacity handling,
error propagation, routing, and lifecycle semantics. It is an overhead
reference, not an apples-to-apples replacement for the framework API.

## Logger comparison with spdlog

The logger component was compared with `spdlog v1.15.3` using its in-memory
`null_sink`, 100,000 formatted records, and a four-worker workload:

```text
micro_single records=100000 elapsed_us=26741 records_per_second=3.73958e+06
spdlog_single records=100000 elapsed_us=6494 records_per_second=1.53988e+07
micro_multi records=100000 elapsed_us=45779 records_per_second=2.18441e+06
spdlog_multi records=100000 elapsed_us=4131 records_per_second=2.42072e+07
```

This is a narrow throughput comparison. `spdlog` uses a null sink, while
MicroErrorSystem creates typed error/log-entry objects and invokes its sink
callback contract. The result identifies an optimization opportunity in the
logger hot path; it does not invalidate the framework's error registration,
routing, policy, or lifecycle features.

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
