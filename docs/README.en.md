# MicroErrorFramework API guide

`MicroErrorFramework` is a header-only C++23 module for describing, categorizing,
and routing errors to category-specific registers. It uses `std::expected` for
operations that need to return either a value or an error.

## Features

- `Error`: an error value with category, code, and message;
- `IRegister`: the register interface;
- `CategoryRegister<Category>`: a base class for one category;
- `Handler`: category-based error routing;
- `Result<T>`: an alias for `std::expected<T, Error>`;
- predefined errors in `vosp::error::predefined`;
- policy-configured `Logger` and `Sink` components;
- bounded `WorkerPool` with 1024 worker/queue limits, backpressure,
  and queued-task cancellation.

For build instructions, validation profiles, and measured results, see the
[project README](../README.md). Precise ownership and concurrency rules are in
the [API contracts](API_CONTRACTS.md).

## Include the module

The module is header-only:

```cpp
#include <vosp.hpp>

using namespace vosp::error;
```

`vosp.hpp` is the single public entry point. The focused headers
`vosp_error.hpp`, `vosp_logger.hpp`, and `vosp_worker_pool.hpp` can be included
separately to reduce compile time.

## Create an error

The constructor takes a category, a stable numeric code, and a human-readable
message:

```cpp
const Error error{
    Category::NETWORK,
    1001,
    "Connection refused"
};

std::cout << error.code() << '\n';
std::cout << error.message() << '\n';

if (error.has_category()) {
    // The error can be routed to a register.
}
```

Errors compare all three fields:

```cpp
const Error same{Category::NETWORK, 1001, "Connection refused"};
const bool equal = error == same;
```

Errors have one stable text representation suitable for diagnostics and logs:

```cpp
#include <format>

const std::string text = to_string(error);
const std::string formatted = std::format("{}", error);
// Both values are: "[NETWORK:1001] Connection refused"
```

Only the empty `std::format` specification is supported. Width, alignment and
other formatter options are deliberately rejected so the serialized diagnostic
form cannot silently change.

## Create registers

For bounded in-memory storage, use `Register`:

```cpp
Register<Category::NETWORK> network;
Register<Category::DATABASE> database;
```

The optional constructor arguments configure the initial reservation and
capacity limit. Custom storage can derive from `CategoryRegister<Category>` and
implement `add()` and `remove()`.

The default `register_policy::ThreadSafe` protects direct concurrent access.
Use `register_policy::SingleThreaded` only when access is externally serialized,
for example by a multi-threaded `System`.

Codes are unique within one category register. Queries return owning values,
never references into locked storage:

```cpp
const auto error = network.find(1001); // std::optional<Error>
const auto current = network.snapshot(); // std::vector<Error>
const OperationResult removed_one = network.remove(1001);
const std::size_t removed = network.clear();
```

`find()` and `snapshot()` remain valid after another thread removes or clears
the corresponding registry entries. Snapshot iteration order is unspecified.

## Route errors with Handler

`Handler` is non-owning. Every register passed to it must outlive the handler.
The first register with a matching category receives the operation:

```cpp
Handler handler{network, database};

const Error error{Category::NETWORK, 1001, "Connection refused"};

const OperationResult result = handler.add(error);
if (!result) {
    // No matching register, or the register rejected the error.
    std::cerr << result.error().message();
}

const OperationResult removed = handler.remove(error);
```

Use one register per category. If several registers have the same category, the
first matching register is selected.

## Select an execution mode

The execution mode is selected at compile time through a dedicated system alias:

```cpp
using Errors = System<
    system_policy::MultiThreaded,
    Register<Category::NETWORK>,
    Register<Category::DATABASE>
>;

Errors system{network, database};
const OperationResult registration_result = system.add(error);
```

Available modes:

- `system_policy::SingleThreaded`: direct routing without handler locks;
- `system_policy::MultiThreaded`: one routing lock per register;
- `system_policy::Async<Executor>`: submission to an external executor with
  `std::future<OperationResult>` results.

Example executor:

```cpp
class Executor
{
public:
    std::future<OperationResult> submit(std::function<OperationResult()> job)
    {
        return std::async(std::launch::async, std::move(job));
    }
};

using NetworkRegister = Register<Category::NETWORK>;
using AsyncErrors = System<system_policy::Async<Executor>, NetworkRegister>;

Executor executor;
AsyncErrors system{executor, network};
std::future<OperationResult> operation = system.add(error);
const OperationResult result = operation.get();
```

An asynchronous `System` does not own the executor or create a hidden thread pool.
The executor and registers must outlive every future created by the system.

For production asynchronous execution, use the built-in pool:

```cpp
vosp::async::WorkerPool pool{4};
using Async = System<system_policy::Async<decltype(pool)>, NetworkRegister>;

Async system{pool, network};
std::future<OperationResult> task = system.add(error);
OperationResult result = task.get();

pool.dispatch([]() -> OperationResult {
    return {};
});
```

The pool reuses fixed workers and preallocated ring-buffer queue slots. Its
queue is bounded to 1024 tasks; `submit()` waits for capacity, and
`clear_queue()` completes pending futures with a cancellation error.
`ShutdownMode::DRAIN` executes accepted work without requesting cancellation.
`CANCEL_PENDING` cancels queued work and requests cooperative cancellation from
active tasks. Running functions are not forcefully interrupted, so callbacks
submitted through `submit_cancellable()` must check their `std::stop_token`.
For work that does not need a future, `dispatch()` avoids promise/future shared
state allocation. `failed_dispatches()` and `cancelled_dispatches()` preserve
operational visibility for that faster path.
`dispatch_bulk(std::span<Task>)` is the throughput-oriented form: it consumes
callbacks in grouped queue refills and returns the exact accepted count.
`Register` also accepts a per-instance `capacity_limit`, bounded by the
global `max_register_capacity`.

## Result<T>

Use `Result<T>` when an operation must return either a value or a detailed
error:

```cpp
Result<int> read_attempts()
{
    if (/* data is available */ true) {
        return 3;
    }

    return std::unexpected(predefined::database_error);
}

const Result<int> result = read_attempts();
if (result) {
    const int attempts = *result;
} else {
    const Error& error = result.error();
}
```

For operations without a value, use `Result<void>`:

```cpp
Result<void> initialize()
{
    return {};
}
```

Use `attempt()` at an exception boundary to preserve the framework's
`std::expected` error channel:

```cpp
const Error context{Category::DATABASE, 2001, "database query"};
Result<int> rows = attempt(context, []() -> int {
    return run_database_query(); // exceptions become Error
});
```

For a `std::exception`, its diagnostic is appended to the context message.
Unknown exceptions retain the supplied `Error` unchanged. A callable returning
a reference is rejected because `std::expected` cannot own a reference value.

## Logging

The logger is separate from registers: registers own error storage, while the
logger publishes events to connected sinks.

```cpp
#include <vosp.hpp>
#include <iostream>

using namespace vosp::logger;

Sink output{std::cout};
Logger logger{output};

const Error error{Category::NETWORK, 1001, "Connection refused"};
const bool logged = logger.error(error);
```

Output:

```text
[ERROR] [NETWORK] code=1001 message=Connection refused
```

`Logger::capture()` combines `attempt()` with best-effort failure logging, and
failed `Result<T>` values can be logged directly:

```cpp
Result<int> rows = logger.capture(
    Error{Category::DATABASE, 2001, "database query"},
    [] { return run_database_query(); });

logger.error(rows); // no-op on success; logs rows.error() on failure
```

Sink rejection or an exception thrown by a sink never replaces the operation
error returned by `capture()`.

The default `Sink` writes every record immediately. For a shared stream with
several producers, select the buffered sink policy:

```cpp
Sink<sink_policy::Buffered> sink{std::cout};
Logger<logger_policy::AcceptAll,
       logger_policy::Parallel,
       logger_policy::MinimalMetadata> logger{sink};

const bool logged = logger.info(
    Error{Category::NETWORK, 1002, "Request completed"});
if (!sink.flush()) {
    // Handle destination stream failure.
}
```

The buffered `Sink` uses a 64 KiB buffer per producer by default. The threshold
is an optional second constructor argument. Flush after producers have joined.
Per-producer order is preserved; cross-producer order is unspecified.

Implement a custom sink by overriding `ILogSink::write()`. A reference-based
sink must outlive the logger. To transfer safe lifetime management to the
logger, pass `std::shared_ptr<ILogSink>` to its constructor or `attach()`;
ownership ends at `detach()` or logger destruction. Sink callbacks run outside
the logger mutex, so reentrant `attach/detach` is supported. Compile-time
filtering is available with
`Logger<logger_policy::Minimum<Level::WARNING>>`.

For a thread-safe high-throughput path, configure the same `Logger` with
`logger_policy::Parallel` and `logger_policy::MinimalMetadata`. This keeps one API,
avoids callback serialization, and omits timestamp/thread-id capture.

`logger_policy::Async` selects the bounded asynchronous specialization of the
same `Logger`. It owns queued `Error` values, applies blocking backpressure at
1,024 pending records, drains batches on one worker, and provides `flush()` plus
`failed_records()` for lifecycle and sink-failure control.

## Predefined errors

The module provides ready-to-use values:

```cpp
using namespace vosp::error::predefined;

const Error& network = network_error;
const Error& database = database_error;
const Error& filesystem = filesystem_error;
const Error& unknown = uncategorized_error;
```

They are declared as `inline const`, require no ownership from the caller, and
can be returned through `std::unexpected`.

## Extending the module

1. Add a category to `Category`.
2. Use `Register<NewCategory>` or derive from
   `CategoryRegister<NewCategory>`.
3. For custom storage, implement `add()` and `remove()`.
4. Pass the register to `Handler`.
5. Add routing tests and update the documentation.

## Current limitations

- `Handler` does not own registers or synchronize access to them;
- `Category::NONE` is not routed to a specialized register;
- `add/remove` return `OperationResult`, so the rejection reason is available
  through `result.error()`;
- `inline constexpr Error` is not used because `Error` stores its message in
  `std::string`.
