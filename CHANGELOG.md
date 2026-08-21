# Changelog

All notable changes are recorded here. The format follows Keep a Changelog and
the version policy described in `docs/API_CONTRACTS.md`.

## [Unreleased]

## [0.3.1-beta] - 2026-08-21

### Performance

- Split scalar and bulk claim storage so scalar dispatch constructs one task
  slot while bulk dispatch initializes its 16-slot batch only when selected.
- Improved the 20-scenario WorkerPool dispatch geometric mean by 15.3% over
  `v0.3.0-beta` in the documented seven-run local Release comparison.

### CI

- Changed the performance workflow to compare every candidate with the immutable
  `v0.3.0-beta` tag on pushes, pull requests, and manual runs.
- Added alternating run order, subsystem geometric means, message-size reporting,
  a 5% dispatch improvement gate, and bounded dispatch/subsystem regression gates.

## [0.3.0-beta] - 2026-08-21

### Changed

- Reworked `WorkerPool` around independent producer/consumer queue ends,
  `std::atomic::wait` backpressure, packed pending/active state and epoch-based
  idle notification.
- Added bounded 16-callback claiming for bulk dispatch while retaining scalar
  cancellation semantics and individual failure accounting.
- Replaced the async logger deque with two pre-reserved vector queues swapped
  between producers and the backend consumer.
- Strengthened `wait()` so completion includes readiness of tracked futures.

### Performance

- Reached a 93.1% geometric-mean WorkerPool dispatch gain over `v0.2.5-beta`
  across the 20-scenario queue/producer/worker matrix on the documented local
  Release setup; native bulk dispatch improved by 225.2%.
- Reduced observed async logger allocation bytes by 19.4% for the 200,000-record
  allocation workload.

### Validation

- Added repeatable legacy-versus-candidate comparison tooling and a manual
  GitHub Actions gate with raw CSV artifacts and a 70% dispatch target.
- Extended WorkerPool stress coverage for repeated waits and tracked-future
  readiness, and added WorkerPool Helgrind execution.

## [0.2.5-beta] - 2026-08-21

### CI

- Added a dedicated public API contract gate that runs positive, negative and
  compile-fail contract tests independently of benchmarks.
- Updated the installed-package gate to enforce the exact `0.2.5` CMake package
  version while continuing to reject test and benchmark artifacts.

## [0.2.4-beta] - 2026-08-21

### Documentation

- Defined the stable source, ownership, formatting, policy-selection and
  compatibility contracts for the complete `0.2.x-beta` release line.
- Documented the positive, negative and compile-fail tests that enforce the
  public API boundary without changing runtime behavior.

## [0.2.3-beta] - 2026-08-21

### Added

- Dedicated negative API tests for unsupported formatting, unroutable errors,
  invalid sink state, null sink ownership, and worker queue limits.

## [0.2.2-beta] - 2026-08-21

### Added

- Cross-platform compile-fail contracts for invalid register, executor, sink,
  and logger dispatch policies.

## [0.2.1-beta] - 2026-08-21

### Added

- Stable `[CATEGORY:code] message` representation through
  `vosp::error::to_string`, `category_name`, and `std::formatter<Error>`.

## [0.2.0-beta] - 2026-08-21

### Added

- Unified policy-based `Register`, `System`, `Sink`, `Logger`, and `WorkerPool`
  API with focused contract tests.
- Explicit concurrency contract tests for full-queue backpressure, cooperative
  cancellation, exception propagation, queue clearing, concurrent/repeated
  shutdown, worker-initiated shutdown and async sink recovery/failure.
- Public-state fuzz transitions covering valid, missing and malformed categories,
  duplicate operations, removal and messages up to 16 KiB.
- Async logger end-to-end and memory-footprint production benchmark scenarios.
- Opt-in legacy-versus-unified API benchmark proving that compatibility aliases
  add no object-size or runtime overhead; benchmark targets remain excluded from
  installed packages.
- Repository-local GCC, Clang, MSVC, TSan, LibFuzzer, Valgrind and clang-tidy CI.
- Scheduled production matrix and configurable multi-hour soak workflow.
- Public API, ownership, blocking and thread-safety contracts.

### Changed

- Documentation now leads with the unified API; the original specialized names
  remain compatibility aliases during the beta migration.
- `Logger` and policy-selected `Sink` are now the implementation types; the
  original logger and sink names are zero-overhead compatibility aliases.
- Windows deterministic sanitizer fuzz documentation now uses RelWithDebInfo to
  avoid mixing dynamic Clang ASan with the Microsoft Debug CRT.

### Fixed

- Linux CI package installation now bypasses a failing Azure Ubuntu mirror and
  uses bounded APT retries and network timeouts through one shared script.
- Minimal CI package installation explicitly includes the Clang 19 sanitizer
  runtime required by ASan, UBSan, and LibFuzzer link steps.
- Linux Clang CI now uses libc++ in its complete experimental-library mode,
  which provides `std::expected`, `std::jthread`, and `std::stop_token` on the
  runner's libc++ 18; configuration fails early with an actionable diagnostic
  for unsupported standard-library combinations.
- Header-only package consumers now inherit the platform thread dependency, and
  the installed CMake package resolves it before importing `vosp::vosp`.
- Minimal logger metadata no longer marks the non-literal `std::thread::id`
  return type `constexpr`, restoring portability across standard libraries.
- The async logger initializes every worker-visible state field before starting
  its worker, eliminating a constructor-time race detected independently by
  ThreadSanitizer and Helgrind.
- Async logger condition-variable notifications now share the state mutex,
  giving Helgrind an explicit happens-before relationship without suppressions.
- Clang 19 ASan/UBSan and LibFuzzer use a matching libstdc++ 14 ABI and the
  standard C++20 Concepts feature-test value required by `<expected>`; normal
  Clang and clang-tidy jobs retain libc++ coverage.

## [0.1.0-beta] - 2026-08-18

### Added

- Header-only C++23 error registers, category routing and expected-based results.
- Serialized, parallel and asynchronous policy loggers with stream sinks.
- Bounded industrial worker pool with backpressure, cancellation and shutdown
  policies.
- Unit, stress, integration, fuzz and benchmark targets.
