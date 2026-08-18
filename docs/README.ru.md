# MicroErrorSystem — документация

`MicroErrorSystem` — header-only модуль для описания ошибок, категоризации и
маршрутизации ошибок в специализированные регистры. Модуль использует C++23
и `std::expected`.

## Возможности

- `Error` — значение ошибки с категорией, кодом и сообщением;
- `IRegister` — интерфейс регистра ошибок;
- `CategoryRegister<Category>` — базовый класс для регистра одной категории;
- `Handler` — маршрутизатор ошибок по категории;
- `Result<T>` — псевдоним `std::expected<T, Error>`;
- предопределённые ошибки в `vosp::error::predefined`;
- `ILogger`, `Logger`, `PolicyLogger`, `ILogSink` и `ConsoleSink` для
  потокобезопасного логирования.

## Требования и сборка

Требуются CMake 3.25+, компилятор C++23 и стандартная библиотека с поддержкой
`std::expected`.

```text
cmake -S MicroErrorSystem -B MicroErrorSystem/cmake-build-debug -DBUILD_TESTING=ON
cmake --build MicroErrorSystem/cmake-build-debug --parallel
ctest --test-dir MicroErrorSystem/cmake-build-debug --output-on-failure
```

## Подключение

Модуль header-only, поэтому достаточно подключить один файл:

```cpp
#include <vosp.hpp>

using namespace vosp::error;
```

`vosp.hpp` — единая публичная точка подключения. Отдельные заголовки
`vosp_error.hpp` и `vosp_logger.hpp` можно подключать для уменьшения времени
компиляции.

## Benchmark и санитайзеры

Benchmark собирается с `-DBUILD_BENCHMARKS=ON` и измеряет операции добавления в
`MemoryRegister`. Для поиска ошибок памяти и неопределённого поведения
используйте Clang/Ninja с `-DENABLE_SANITIZERS=ON`.

Fuzzing API доступен опционально на Linux/macOS с Clang:

```text
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/cmake-build-fuzz -DBUILD_FUZZERS=ON
cmake --build MicroErrorSystem/cmake-build-fuzz --parallel
MicroErrorSystem/cmake-build-fuzz/MicroErrorSystemFuzzer -runs=10000
```

Для интеграционного стресс-теста используется внешний MIT-проект
[`nlohmann/json`](https://github.com/nlohmann/json), зафиксированный на
`v3.12.0`. Он подключается только при `-DBUILD_EXTERNAL_STRESS_TESTS=ON` и не
становится зависимостью runtime-библиотеки:

```text
cmake -S MicroErrorSystem -B MicroErrorSystem/cmake-build-external -DBUILD_TESTING=ON -DBUILD_EXTERNAL_STRESS_TESTS=ON
cmake --build MicroErrorSystem/cmake-build-external --parallel
ctest --test-dir MicroErrorSystem/cmake-build-external -R NlohmannStressTests --output-on-failure
```

## Создание ошибки

Конструктор принимает категорию, код и человекочитаемое сообщение:

```cpp
const Error error{
    Category::NETWORK,
    1001,
    "Connection refused"
};

std::cout << error.code() << '\n';
std::cout << error.message() << '\n';

if (error.has_category()) {
    // Ошибка готова к маршрутизации.
}
```

Ошибки сравниваются по всем трём полям:

```cpp
const Error same{Category::NETWORK, 1001, "Connection refused"};
const bool equal = error == same;
```

## Создание регистра

`CategoryRegister` автоматически реализует `category()`. Конкретному регистру
нужно реализовать только `add()` и `remove()`:

```cpp
class NetworkRegister final : public CategoryRegister<Category::NETWORK>
{
public:
    OperationResult add(const Error& error) override
    {
        errors_.push_back(error);
        return {};
    }

    OperationResult remove(const Error& error) override
    {
        // Реальная реализация должна найти и удалить error из хранилища.
        return {};
    }

private:
    std::vector<Error> errors_;
};
```

Регистр владеет своим хранилищем и определяет правила дубликатов, удаления и
потокобезопасности.

## Маршрутизация через Handler

`Handler` не владеет регистрами. Переданные регистры должны жить дольше него.
При вызове выбирается первый регистр с категорией ошибки:

```cpp
NetworkRegister network;
DatabaseRegister database;

Handler handler{network, database};

const Error error{Category::NETWORK, 1001, "Connection refused"};

const OperationResult result = handler.add(error);
if (!result) {
    // Подходящий регистр не найден или отклонил ошибку.
    std::cerr << result.error().message();
}

handler.remove(error);
```

Для одной категории рекомендуется передавать только один регистр. Если
совпадений несколько, используется первый.

## Выбор режима выполнения

Режим выбирается готовым псевдонимом типа на этапе компиляции:

```cpp
using System = MultiThreadedSystem<
    NetworkRegister,
    DatabaseRegister
>;

System system{network, database};
system.add(error);
```

Доступны три режима:

- `SingleThreadedRegister` + `SingleThreadedHandler` — без блокировок;
- `MultiThreadedRegister` + `MultiThreadedHandler` — операции защищены mutex;
- `AsyncRegister<Executor>` + `AsyncHandler<Executor>` — операции выполняются
  внешним executor и возвращают `std::future<OperationResult>`.

Пример асинхронного executor:

```cpp
class Executor
{
public:
    std::future<OperationResult> submit(std::function<OperationResult()> job)
    {
        return std::async(std::launch::async, std::move(job));
    }
};

using System = AsyncSystem<Executor, NetworkRegister>;

Executor executor;
System system{executor, network};
std::future<OperationResult> operation = system.add(error);
const OperationResult result = operation.get();
```

`AsyncSystem` не владеет executor и не создаёт скрытый пул потоков. Executor и
регистры должны жить дольше всех futures, созданных системой.

Для промышленного асинхронного режима используется готовый пул:

```cpp
vosp::async::IndustrialWorkerPool pool{4};
using Async = AsyncSystem<decltype(pool), NetworkRegister>;

Async system{pool, network};
std::future<OperationResult> task = system.add(error);
OperationResult result = task.get();
```

Пул повторно использует фиксированные worker-потоки. Очередь ограничена 1024
задачами; при заполнении `submit()` блокируется до освобождения места. Лимит
worker-потоков также равен 1024:

```cpp
vosp::async::IndustrialWorkerPool pool{4, 128};
const std::size_t cancelled = pool.clear_queue();
pool.shutdown(vosp::async::ShutdownMode::CANCEL_PENDING);
```

`clear_queue()` завершает futures ожидающих задач ошибкой с кодом
`task_cancelled_code`. Уже выполняемая функция не прерывается принудительно:
для этого нужен cooperative cancellation в самой функции. Для этого есть
отдельный API:

```cpp
auto task = pool.submit_cancellable([](std::stop_token stop) -> OperationResult {
    while (!stop.stop_requested()) {
        // Выполняем небольшую порцию работы.
    }
    return std::unexpected(vosp::async::task_cancelled_error());
});
```

`MemoryRegister` принимает второй параметр `capacity_limit`; он ограничивает
число ошибок конкретного регистра и не может превышать
`max_register_capacity`.

## Result<T>

`Result<T>` используется, когда операции нужно вернуть значение или подробную
ошибку:

```cpp
Result<int> read_attempts()
{
    if (/* данные доступны */ true) {
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

Для `void` используется `Result<void>`:

```cpp
Result<void> initialize()
{
    return {};
}
```

## Логирование

Logger отделён от регистров: регистр отвечает за хранение, а logger — за
публикацию событий в подключённые sink.

```cpp
#include "vosp_logger.hpp"
#include <iostream>

using namespace vosp::logger;

ConsoleSink console{std::cout};
Logger logger{console};

const Error error{Category::NETWORK, 1001, "Connection refused"};
logger.error(error);
```

Вывод:

```text
[ERROR] [NETWORK] code=1001 message=Connection refused
```

Собственный sink реализует `ILogSink::write()`. Logger не владеет sink, а все
его методы можно вызывать из нескольких потоков. Callback sink выполняется
вне внутреннего mutex, поэтому sink может вызвать `attach/detach`; его lifetime
всё равно должен превышать lifetime logger.

Для compile-time фильтрации используйте policy:

```cpp
PolicyLogger<MinimumLevelPolicy<Level::WARNING>> production_logger{console};
production_logger.info(error);    // запись отброшена
production_logger.warning(error); // запись передана в sink
```

## Предопределённые ошибки

Доступны готовые значения:

```cpp
using namespace vosp::error::predefined;

const Error& network = network_error;
const Error& database = database_error;
const Error& filesystem = filesystem_error;
const Error& unknown = uncategorized_error;
```

Они объявлены как `inline const`, не владеются пользователем и подходят для
возврата через `std::unexpected`.

## Расширение модуля

1. Добавьте новую категорию в `Category`.
2. Создайте класс `CategoryRegister<НоваяКатегория>`.
3. Реализуйте `add()` и `remove()`.
4. Передайте регистр в `Handler`.
5. Добавьте тесты маршрутизации и обновите документацию.

## Ограничения

- `Handler` не владеет регистрами и не синхронизирует доступ к ним;
- `Category::NONE` не маршрутизируется в специализированный регистр;
- `add/remove` возвращают `OperationResult`, поэтому причина отказа доступна
  через `result.error()`;
- `inline constexpr Error` не используется, поскольку `Error` хранит сообщение
  в `std::string`.
