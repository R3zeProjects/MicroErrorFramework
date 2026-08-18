# Changelog

All notable changes are recorded here. The format follows Keep a Changelog and
the version policy described in `docs/API_CONTRACTS.md`.

## [Unreleased]

### Added

- Explicit concurrency contract tests for full-queue backpressure, cooperative
  cancellation, exception propagation, queue clearing, concurrent/repeated
  shutdown, worker-initiated shutdown and async sink recovery/failure.
- Public-state fuzz transitions covering valid, missing and malformed categories,
  duplicate operations, removal and messages up to 16 KiB.
- Async logger end-to-end and memory-footprint production benchmark scenarios.
- Repository-local GCC, Clang, MSVC, TSan, LibFuzzer, Valgrind and clang-tidy CI.
- Scheduled production matrix and configurable multi-hour soak workflow.
- Public API, ownership, blocking and thread-safety contracts.

### Changed

- Windows deterministic sanitizer fuzz documentation now uses RelWithDebInfo to
  avoid mixing dynamic Clang ASan with the Microsoft Debug CRT.

### Fixed

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
- Clang ASan/UBSan and LibFuzzer use a matching libstdc++ 14 ABI; normal Clang
  and clang-tidy jobs retain libc++ coverage.

## [0.1.0-beta] - 2026-08-18

### Added

- Header-only C++23 error registers, category routing and expected-based results.
- Serialized, parallel and asynchronous policy loggers with stream sinks.
- Bounded industrial worker pool with backpressure, cancellation and shutdown
  policies.
- Unit, stress, integration, fuzz and benchmark targets.
