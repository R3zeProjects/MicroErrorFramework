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

## [0.1.0-beta] - 2026-08-18

### Added

- Header-only C++23 error registers, category routing and expected-based results.
- Serialized, parallel and asynchronous policy loggers with stream sinks.
- Bounded industrial worker pool with backpressure, cancellation and shutdown
  policies.
- Unit, stress, integration, fuzz and benchmark targets.
