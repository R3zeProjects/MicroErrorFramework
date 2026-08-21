# Руководство API MicroErrorSystem

## Подключение

```cpp
#include <vosp.hpp>
```

`vosp.hpp` подключает error, logger, worker-pool и version API. Для уменьшения
compile time доступны специализированные headers.

## Error и Result

```cpp
using namespace vosp::error;

const Error error{Category::NETWORK, 1001, "Connection refused"};
Result<int> attempts = 3;
```

`Error` сравнивается по category, code и message. `Result<T>` — это
`std::expected<T, Error>`.

## Register

```cpp
Register<Category::NETWORK> network;
const OperationResult added = network.add(error);
```

Register проверяет category, duplicate, missing error и capacity. Default policy
потокобезопасна. `register_policy::SingleThreaded` используется только при
внешней синхронизации.

## System

```cpp
using Errors = System<
    system_policy::MultiThreaded,
    decltype(network)>;

Errors errors{network};
const OperationResult result = errors.add(error);
```

Async-вариант:

```cpp
vosp::async::WorkerPool workers{4};
using AsyncErrors = System<
    system_policy::Async<vosp::async::WorkerPool>,
    decltype(network)>;

AsyncErrors errors{workers, network};
auto future = errors.add(error);
const OperationResult result = future.get();
```

## Logger и Sink

```cpp
using namespace vosp::logger;

Sink output{std::cout};
Logger logger{output};
const bool logged = logger.error(error);
```

Buffered parallel mode:

```cpp
Sink<sink_policy::Buffered> output{std::cout};
Logger<logger_policy::AcceptAll,
       logger_policy::Parallel,
       logger_policy::MinimalMetadata> logger{output};

const bool logged = logger.info(error);
const bool flushed = output.flush();
```

Async mode выбирается `logger_policy::Async`. Его `flush()` ждёт обработки
accepted records, а `failed_records()` возвращает число sink failures.

## WorkerPool

`WorkerPool` предоставляет tracked `submit`, fire-and-forget `dispatch`, batch
`dispatch_bulk`, cooperative cancellation, queue cleanup и два shutdown mode.
Queue bounded и применяет backpressure.

## Расширение

- custom storage реализует `CategoryRegister<NewCategory>`;
- custom destination реализует `ILogSink`;
- custom executor удовлетворяет `AsyncExecutor`;
- custom policies удовлетворяют соответствующим concepts.

См. [миграцию](API_MIGRATION.ru.md), [архитектуру](ARCHITECTURE.ru.md) и
[контракты](API_CONTRACTS.ru.md).
