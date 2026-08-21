# MicroErrorSystem architecture

## Scope

MicroErrorSystem is a header-only C++23 micro-framework for building an error
control and logging boundary around an application. It provides:

- owned error values and category-based routing;
- bounded in-memory error registers;
- synchronous, synchronized, and externally scheduled error systems;
- policy-based synchronous and asynchronous logging;
- a bounded worker pool with backpressure and cooperative cancellation.

The framework does not define application-specific recovery, persistent
storage, log rotation, distributed transport, or process-wide global state.

## Design goals

- Keep the default API small and available through `<vosp.hpp>`.
- Make ownership and shutdown behavior explicit.
- Bound framework-managed queues and storage.
- Select synchronization and metadata costs at compile time.
- Keep error routing independent from logging and task scheduling.

## Components

### Error model

`vosp::error::Error` owns a category, numeric code, and message. Equality
compares all three fields. `Result<T>` is an alias for
`std::expected<T, Error>`, and `OperationResult` represents an operation that
returns no value on success.

### Registers and routing

`IRegister` defines the category-specific `add`, `remove`, and `category`
contract. `CategoryRegister<Category>` supplies the category implementation for
custom registers.

`Register<Category, Policy>` is the built-in bounded implementation. It uses
`std::unordered_set` storage, rejects errors from another category, and reports
duplicate, missing, and capacity failures through `OperationResult`.
`register_policy::ThreadSafe` is the default; `SingleThreaded` removes internal
locking when access is externally serialized.

`Handler` routes an operation to the first matching register without adding
synchronization. `ConcurrentHandler` adds one independent lock per register.
Both handlers reference externally owned registers.

### Error systems

`System<Policy, Registers...>` selects execution behavior at compile time:

- `system_policy::SingleThreaded` performs direct calls without handler locks;
- `system_policy::MultiThreaded` serializes operations independently per register;
- `system_policy::Async<Executor>` submits owned error values to an external
  executor and returns `std::future<OperationResult>`.

The asynchronous system shares its handler with submitted callbacks so that a
callback never captures a destroyed `System` object. The executor and
referenced registers remain externally owned.

### Logging

`Logger` sends `LogEntry` values to one or more `ILogSink` instances. Its
`logger_policy` parameters select level filtering, metadata capture, and
dispatch mode. `logger_policy::Async` adds a bounded queue,
blocking backpressure, batch delivery, explicit `flush`, and failure counters.

Sinks may be attached by reference or by `std::shared_ptr`. Reference-attached
sinks are non-owning; shared sinks remain owned until detachment or logger
destruction. Sink callbacks execute outside the sink-list mutex.

`Sink<sink_policy::Immediate>` writes each record immediately. The buffered
policy maintains a buffer per producer and serializes only stream writes.
Explicit `flush()` is required when the caller needs final delivery status.

### Worker pool

`WorkerPool` owns a fixed set of `std::jthread` workers and a
preallocated ring queue. Both worker count and queue capacity are bounded by
1,024. A full queue applies blocking backpressure until capacity becomes
available or shutdown starts.

Tracked `submit` operations return futures. Fire-and-forget `dispatch`
operations avoid promise/future shared state and expose aggregate failure and
cancellation counters. Cancellable callbacks receive `std::stop_token` and
must cooperate by checking it.

### Public entry point

`<vosp.hpp>` includes the error, logger, worker-pool, and version APIs. Focused
headers remain available when compile-time isolation is useful.

## Control flow

Error registration:

```text
Error -> ErrorSystem/Handler -> category match -> IRegister -> OperationResult
```

Asynchronous logging:

```text
Error -> Logger -> bounded queue -> logger worker -> Sink
```

The two flows are intentionally independent. An application may register an
error, log it, do both, or do neither.

## Ownership and lifetime

- `Error` and queued asynchronous records own their message storage.
- Handlers and error systems do not own registers.
- An asynchronous `System` does not own its executor.
- A reference-attached sink must outlive all logger calls that can reach it.
- A shared sink is retained by the logger while attached.
- Stream sinks do not own their `std::ostream`.
- `WorkerPool` owns and joins its workers.

For asynchronous error operations, the executor and registers must outlive all
submitted operations and their futures. For buffered output, producer threads
must finish before the final explicit flush and stream destruction.

## Concurrency and shutdown invariants

- A `System` with `SingleThreaded` policy requires external serialization.
- The default `Register` policy synchronizes its own storage.
- Multi-threaded and asynchronous system policies serialize routing per register.
- Parallel logger dispatch requires every attached sink to support concurrent
  `write` calls.
- `ShutdownMode::DRAIN` accepts no new work and runs all queued tasks.
- `ShutdownMode::CANCEL_PENDING` cancels queued tasks and requests cooperative
  cancellation from active cancellable tasks.
- A worker may request pool shutdown, but only an owning external thread joins
  the workers; this prevents self-join deadlocks.

## Extension points

- Add an application category and derive its storage from
  `CategoryRegister<NewCategory>`.
- Provide an executor whose `submit` operation satisfies `AsyncExecutor`.
- Implement `ILogSink::write` for a new logging destination.
- Define logger filtering, metadata, or dispatch policies that satisfy the
  corresponding concepts.

## Operational limits

- Routing uses the first register with a matching category; configure at most
  one register per category.
- `Category::NONE` is not routed to a specialized register.
- Register capacity is capped by `max_register_capacity`.
- Worker and asynchronous logger queues are bounded and apply backpressure.
- Running tasks cannot be forcefully interrupted safely.
- Cross-producer ordering in a buffered `Sink` is unspecified.
- Predefined errors are `inline const` because `Error` owns a `std::string` and
  is not a literal `constexpr` type.

See [API contracts](API_CONTRACTS.md) for precise lifetime and shutdown rules,
and [the API guide](README.en.md) for usage examples.
