# MicroErrorFramework

**MicroErrorFramework** — header-only микро-фреймворк на C++23 для построения
независимого контура контроля ошибок, логгирования и асинхронного выполнения.

> Один компактный API, а режимы хранения, маршрутизации и логгирования
> выбираются compile-time политиками.

![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![API](https://img.shields.io/badge/API-0.2.5--beta-orange)
![CMake](https://img.shields.io/badge/CMake-3.25%2B-064F8C)
![License](https://img.shields.io/badge/license-MIT-green)

## Основной API

| Ответственность | Основной тип | Варианты |
| --- | --- | --- |
| Значение ошибки | `Error` | категория, код, сообщение |
| Результат операции | `Result<T>` | `std::expected<T, Error>` |
| Хранение | `Register<Category, Policy>` | thread-safe / single-threaded |
| Маршрутизация | `System<Policy, Registers...>` | single / multi / async |
| Вывод логов | `Sink<Policy>` | immediate / buffered |
| Логирование | `Logger<Filter, Dispatch, Metadata>` | serialized / parallel / async |
| Выполнение задач | `WorkerPool` | bounded queue, cancellation, shutdown |

Старые имена (`MemoryRegister`, `PolicyLogger`, `ConsoleSink`,
`IndustrialWorkerPool` и системные aliases) временно сохранены для совместимости
beta-версии. Новый код должен использовать единый API.

## Возможности

- владеющий `Error` с типизированной категорией;
- `Result<T>` и `OperationResult` на основе `std::expected`;
- bounded-регистры с проверкой категории, дубликатов и capacity;
- direct, synchronized и asynchronous маршрутизация;
- logger с compile-time фильтрацией, metadata и dispatch policy;
- immediate и producer-buffered stream sink;
- bounded `WorkerPool` до 1 024 workers и 1 024 queued tasks;
- blocking backpressure и cooperative cancellation через `std::stop_token`;
- `DRAIN`/`CANCEL_PENDING`, очистка очереди и безопасный shutdown;
- внешнее или shared-владение sink-ами;
- CTest, stress tests, ASan/UBSan, TSan, Valgrind, LibFuzzer и clang-tidy;
- проверка GCC, Clang и MSVC в GitHub Actions.

## Требования

- CMake 3.25+;
- компилятор C++23;
- стандартная библиотека с `std::expected`, `std::jthread` и `std::stop_token`.

## Подключение

```cpp
#include <vosp.hpp>
```

После установки CMake package:

```cmake
find_package(vosp 0.1 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE vosp::vosp)
```

## Минимальный пример

```cpp
#include <vosp.hpp>
#include <iostream>

using namespace vosp::error;

int main()
{
    Register<Category::NETWORK> network;
    System<system_policy::MultiThreaded, decltype(network)> errors{network};

    vosp::logger::Sink output{std::cout};
    vosp::logger::Logger logger{output};

    const Error error{Category::NETWORK, 1001, "Connection refused"};
    const OperationResult registered = errors.add(error);
    if (!registered)
    {
        return 1;
    }

    return logger.error(error) ? 0 : 2;
}
```

## Политики регистра и системы

По умолчанию `Register` защищает своё хранилище mutex-ом:

```cpp
Register<Category::NETWORK> network;
```

Если доступ уже сериализован `System`, внутренний mutex можно убрать:

```cpp
using NetworkRegister =
    Register<Category::NETWORK, register_policy::SingleThreaded>;

NetworkRegister network;
System<system_policy::MultiThreaded, NetworkRegister> errors{network};
```

Такой register нельзя конкурентно вызывать напрямую в обход `System`.

Асинхронный режим принимает внешний executor:

```cpp
vosp::async::WorkerPool workers{4, 128};
Register<Category::DATABASE> database;

using AsyncErrors = System<
    system_policy::Async<vosp::async::WorkerPool>,
    decltype(database)>;

AsyncErrors errors{workers, database};
auto future = errors.add(Error{Category::DATABASE, 2001, "Query failed"});
const OperationResult result = future.get();
workers.shutdown(vosp::async::ShutdownMode::DRAIN);
```

Executor и registers должны жить дольше всех отправленных операций.

## Sink и Logger

Immediate sink выводит запись сразу:

```cpp
vosp::logger::Sink output{std::cout};
vosp::logger::Logger logger{output};
```

Buffered sink уменьшает contention между producer-потоками:

```cpp
using namespace vosp::logger;

Sink<sink_policy::Buffered> output{std::cout};
Logger<logger_policy::AcceptAll,
       logger_policy::Parallel,
       logger_policy::MinimalMetadata> logger{output};

const bool logged = logger.info(
    vosp::error::Error{
        vosp::error::Category::NETWORK, 1002, "Request completed"});
const bool flushed = output.flush();
```

Асинхронный logger использует тот же тип:

```cpp
Logger<logger_policy::AcceptAll,
       logger_policy::Async,
       logger_policy::MinimalMetadata> logger{output};

const bool queued = logger.error(error);
logger.flush();
logger.shutdown();
```

Очередь bounded до 1 024 records и применяет blocking backpressure. Результат
асинхронного `write` означает принятие в очередь; ошибки sink доступны через
`failed_records()`.

## WorkerPool

```cpp
vosp::async::WorkerPool pool{4, 128};

auto future = pool.submit([]() -> vosp::error::OperationResult {
    return {};
});

pool.dispatch([]() -> vosp::error::OperationResult {
    return {};
});

const auto result = future.get();
pool.shutdown(vosp::async::ShutdownMode::DRAIN);
```

- `submit()` возвращает future;
- `dispatch()` не создаёт promise/future shared state;
- `dispatch_bulk()` уменьшает producer-side lock contention;
- `clear_queue()` отменяет ещё не запущенные задачи;
- `CANCEL_PENDING` запрашивает cooperative stop у активных cancellable-задач;
- worker может сигнализировать shutdown, но финальный join выполняет владелец.

## Проверка

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Локально полный набор прошёл `10/10` CTest на MSVC и Clang, а выделенный
API-contract gate — `7/7`. Полный production benchmark также завершён успешно.
Результаты зависят от железа, toolchain и workload; методика находится в
benchmark-документации.

## Документация

- [Русское руководство API](docs/README.ru.md)
- [Русская архитектура](docs/ARCHITECTURE.ru.md)
- [Русские контракты API](docs/API_CONTRACTS.ru.md)
- [Миграция на единый API](docs/API_MIGRATION.ru.md)
- [English API guide](docs/README.en.md)
- [Benchmarks](docs/BENCHMARKS.md)
- [Changelog](CHANGELOG.md)

## Лицензия

Проект распространяется по [MIT License](LICENSE).
