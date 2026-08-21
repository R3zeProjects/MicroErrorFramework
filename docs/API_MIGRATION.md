# Unified API migration

## Goal

The unified API keeps one primary name per responsibility and moves behavioral
variation into compile-time policies. The underlying storage, logging, and
worker hot paths are unchanged.

## Preferred names

| Previous name | Unified API |
| --- | --- |
| `MemoryRegister<Category>` | `Register<Category>` |
| `SingleThreadedSystem<...>` | `System<system_policy::SingleThreaded, ...>` |
| `MultiThreadedSystem<...>` | `System<system_policy::MultiThreaded, ...>` |
| `AsyncSystem<Executor, ...>` | `System<system_policy::Async<Executor>, ...>` |
| `ConsoleSink` | `Sink<>` or CTAD: `Sink sink{stream}` |
| `BufferedStreamSink` | `Sink<sink_policy::Buffered>` |
| `PolicyLogger<...>` | `Logger<...>` |
| `ParallelLogger` | `Logger<AcceptAll, Parallel>` |
| `IndustrialWorkerPool` | `WorkerPool` |

The previous names remain available as compatibility aliases during the beta
migration. New code and documentation should use the unified names.

## Policy groups

- `register_policy`: internal register synchronization;
- `system_policy`: direct, synchronized, or externally scheduled routing;
- `sink_policy`: immediate or producer-buffered stream output;
- `logger_policy`: level filtering, sink dispatch, and metadata capture.

Policies are stateless types. They add no runtime mode branch and no per-object
configuration storage.

## Example

```cpp
#include <vosp.hpp>
#include <iostream>

using namespace vosp::error;

Register<Category::NETWORK> network;
System<system_policy::MultiThreaded, decltype(network)> errors{network};

vosp::logger::Sink output{std::cout};
vosp::logger::Logger logger{output};

const Error error{Category::NETWORK, 1001, "Connection refused"};
const OperationResult registered = errors.add(error);
const bool logged = logger.error(error);
```

For a register without internal locking, used only behind an externally
synchronized system:

```cpp
using NetworkRegister =
    Register<Category::NETWORK, register_policy::SingleThreaded>;
NetworkRegister network;
System<system_policy::MultiThreaded, NetworkRegister> errors{network};
```

Do not access that register directly from concurrent threads; the system owns
only synchronization, not the register lifetime.
