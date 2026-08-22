# Public API and concurrency contracts

## Status and compatibility

The current release line is `0.4.x-beta`. The public API is every declaration
reachable from `include/vosp.hpp`; implementation details under `detail` are not
public. Before 1.0, minor releases may change source compatibility when the
change is recorded in `CHANGELOG.md`. Patch releases remain source compatible:
they may add opt-in APIs and validation, but do not remove declarations or
change the documented semantics of existing calls.

The umbrella header exposes `vsp` as a compact namespace facade for `vosp` and
provides `vsp::Logger` as the short name of `vosp::logger::Logger`. Canonical
`vosp` names remain supported and denote the same types.

The project does not promise a stable binary ABI before 1.0. It is header-only,
so all consumers must rebuild after an upgrade. A 1.0 release requires a
documented API freeze, green supported-compiler CI, Linux concurrency gates and
release-candidate soak evidence.

## Stable 0.4.x source contracts

- `<vosp.hpp>` is the umbrella source entry point in both the source tree and
  the installed package.
- `Error` owns its message and compares category, numeric code and message.
  `category_name()` returns stable uppercase category names and
  `to_string(error)` returns `[CATEGORY:code] message`.
- `std::formatter<Error>` produces the same representation as `to_string()`.
  The only supported format specification is the empty one (`{}`); unsupported
  specifications are rejected with `std::format_error`.
- `Result<T>` is `std::expected<T, Error>` and `OperationResult` is
  `Result<void>`. Expected operational failures use these result types rather
  than exceptions.
- `attempt(error, function)` accepts a nullary callable returning a value or
  `void`. It converts `std::exception` into the supplied category/code with an
  appended diagnostic message and maps unknown exceptions to the supplied Error.
- A register owns at most one Error for each numeric code. `find()` and
  `snapshot()` return owning copies that remain valid after concurrent mutation.
- `Register`, `System`, `Sink`, and `Logger` are the primary policy-selected
  types. The specialized names retained for migration are exact aliases and do
  not add storage, allocation, virtual dispatch or runtime mode selection.
- Register, system, sink and logger policies are selected at compile time and
  constrained by concepts. Unsupported policies are intentionally ill-formed.
- Benchmark and third-party integration targets are opt-in repository tooling;
  they are not part of the installed package or its transitive dependencies.

## Contract verification

The repository verifies the public boundary at three levels:

- positive unit and unified-API tests compile and exercise supported usage;
- negative runtime tests cover rejected formatter specifications, unroutable
  errors, invalid sinks and invalid queue limits;
- compile-fail targets prove that unsupported register, executor, sink and
  logger policies do not satisfy their public concepts.

CTest labels the latter two groups `api-contract`; compile-fail cases additionally
use `compile-fail`. The installed-package consumer builds against only the
generated CMake package and installed headers, which prevents repository-local
includes from hiding packaging defects.

## Ownership and lifetime

- `Error` owns its message. Copies are independent; `message()` is a view valid
  until that `Error` is destroyed or assigned.
- `Register` owns stored `Error` values.
- `Handler`, `ConcurrentHandler`, and synchronous `ErrorSystem` instances hold
  references to registers. Registers must outlive them.
- An asynchronous `System` holds a reference to its executor and shared routing
  state. The executor and registers must outlive all submitted operations and
  the system.
- A logger sink attached by reference must outlive the logger and every
  in-flight callback. `detach()` prevents future selection but is not a lifetime
  barrier for a callback that already selected the sink.
- A sink attached with `shared_ptr` is retained while registered and while an
  in-flight delivery snapshot references it.
- `WorkerPool` owns queued callables and worker threads. Destroying
  the pool from one of its own callbacks is unsupported; an owner thread must
  perform destruction after workers have returned.

## Thread-safety matrix

| Type | Concurrent operations | Contract |
| --- | --- | --- |
| `Error` | const reads | safe after publication; no mutating API |
| default `Register` | `add`, `remove(error/code)`, `contains`, `find`, `snapshot`, `clear`, `size`, `reserve` | internally serialized; observations own their values |
| single-threaded `System` | none | caller provides synchronization |
| multi-threaded `System` | `add`, `remove` | one routing lock per configured register |
| `WorkerPool` | submission, observation, `wait`, `clear_queue`, `shutdown` | safe; shutdown is idempotent and serializes joining |
| serialized `Logger` | publish, attach, detach | safe; sink callbacks are serialized |
| parallel `Logger` | publish, attach, detach | logger state is safe; every sink must itself accept concurrent callbacks |
| async `Logger` | publish, attach, detach, `flush`, `shutdown` | safe; one backend consumer drains a bounded queue |
| immediate `Sink` | `write` | do not mutate the stream concurrently outside the sink |
| buffered `Sink` | `write`, `flush` | producer-local buffers plus serialized output |

## Worker-pool contract

`WorkerPool(worker_count, queue_capacity)` accepts values in
`[1, 1024]`; zero workers means `hardware_concurrency()` and is rejected if the
platform also reports zero. Queue capacity is the maximum number of tasks that
have not started. Active tasks do not consume queue slots.

### Submission and backpressure

- `submit()` and `dispatch()` block while the bounded queue is full.
- They wake when a worker releases a slot or shutdown begins.
- Submission after shutdown throws `std::runtime_error`.
- `submit()` returns a future and may allocate promise/shared-state storage.
- `dispatch()` has no future; a false `OperationResult` or exception increments
  `failed_dispatches()`.
- `dispatch_bulk()` moves accepted callbacks and returns early with the accepted
  count if shutdown begins.
- `dispatch_bulk()` work may be claimed by a worker in chunks of at most 16.
  Once a chunk is claimed it is active and is not removed by `clear_queue()`;
  callbacks still execute individually and failures are counted individually.
- The pool takes ownership of a callable only after successful enqueue.

### Cancellation and shutdown

- `DRAIN` rejects new work, executes every accepted queued task and joins all
  workers. It does not request cooperative stop for active cancellable tasks.
- `CANCEL_PENDING` rejects new work, completes queued futures with
  `task_cancelled_code`, counts cancelled fire-and-forget tasks, requests stop
  for active cancellable tasks, and then joins workers.
- A normal active `Task` cannot be forcibly interrupted.
- `clear_queue()` cancels waiting tasks but leaves active tasks and the pool
  running.
- Calls to `shutdown()` are idempotent and may be concurrent.
- A worker may signal shutdown; it never joins itself. Final joining and object
  destruction remain the owner's responsibility.
- `wait()` blocks until both queued and active counts reach zero and tracked
  futures for completed tasks have been made ready. It does not initiate
  shutdown.

`queue_storage_bytes()` reports reserved ring-slot storage only. Worker stacks,
the pool object and heap allocations made by large callables are excluded.

## Logger contract

### Return values and failures

- Synchronous logger `write()` returns true only if every selected sink returns
  true. Sink exceptions propagate to the caller.
- An async logger return value reports queue acceptance, not final sink success.
- Async sink false results and exceptions are contained and counted by
  `failed_records()`.
- Async `flush()` blocks until every accepted record has reached sink callbacks.
  It does not flush sink-specific buffers; call buffered `Sink::flush()`
  afterwards when stream visibility is required.
- Async publish blocks at the fixed 1024-record queue limit and returns false if
  shutdown begins while it is waiting.
- Async `shutdown()` drains accepted records, rejects new records and is
  idempotent. A publish after shutdown returns false.

### Allocation and blocking notes

- Compile-time filtered records return true without constructing a `LogEntry` or
  calling a sink.
- No general publish API is promised allocation-free: copying an owned message,
  growing sink snapshots, async queue growth, stream formatting and custom sink
  code may allocate.
- A single externally owned sink uses the logger fast path without copying the
  sink list.
- Serialized dispatch can block behind another sink callback. Parallel dispatch
  shifts synchronization responsibility to the sink. Async dispatch can block
  at queue capacity, during `flush()`, or during `shutdown()`.

## Errors and exceptions

Recoverable domain failures use `std::expected<T, Error>`. Stable framework
codes include duplicate, missing error, missing register, capacity, category
mismatch and task cancellation. Allocation failures, invalid constructor
arguments, submission after shutdown and exceptions thrown by user callbacks
use C++ exceptions as documented above.

No API promises operation under global allocation failure. Tests inject sink and
task exceptions; ASan/UBSan, TSan, LibFuzzer, Memcheck and Helgrind are CI gates.

## Operational limitations

- A hung sink can delay serialized callers, async flush and shutdown. VOSP does
  not terminate user callbacks or implement an I/O timeout.
- Immediate and buffered `Sink` policies expose stream failure, but file
  rotation, fsync policy, disk quotas and retention are application-owned.
- Benchmark null streams isolate framework overhead and are not storage latency
  measurements.
