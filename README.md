# MicroErrorSystem

Документация: [русский](docs/README.ru.md) · [English](docs/README.en.md) · [简体中文](docs/README.zh-CN.md)

API version: `0.1.0` · [Changelog](../CHANGELOG.md) · [Benchmark report](docs/BENCHMARKS.md)

Заготовка модульной системы регистрации ошибок на C++23.

## Возможности

- типизированная ошибка `vosp::error::Error`;
- категории `NETWORK`, `DATABASE`, `FILESYSTEM`;
- интерфейс регистра `IRegister`;
- маршрутизация ошибок через `Handler`;
- compile-time режимы `SingleThreaded`, `MultiThreaded` и `Async`;
- потокобезопасный logger с подключаемыми sink;
- единая точка подключения `vosp.hpp`;
- benchmark и опциональные ASan/UBSan-проверки;
- опциональная LibFuzzer-цель для Linux/macOS с Clang;
- opt-in интеграционный стресс-тест с внешним `nlohmann/json`;
- `IndustrialWorkerPool` с фиксированным числом worker-потоков;
- bounded-очередь с backpressure и лимитами до 1024 workers/queued tasks;
- реальная отмена ожидающих задач через `clear_queue()` и `CANCEL_PENDING`;
- compile-time policy-фильтр логгера;
- `Result<T> = std::expected<T, Error>`;
- предопределённые ошибки в `vosp::error::predefined`;
- тесты через CTest без внешних зависимостей.

## Требования

- CMake 3.25 или новее;
- компилятор с поддержкой C++23;
- стандартная библиотека с `std::expected`.

## Сборка

Из корня репозитория:

```text
cmake -S MicroErrorSystem -B MicroErrorSystem/cmake-build-debug -DBUILD_TESTING=ON
cmake --build MicroErrorSystem/cmake-build-debug --parallel
```

Benchmark:

```text
cmake -S MicroErrorSystem -B MicroErrorSystem/cmake-build-debug -DBUILD_BENCHMARKS=ON
cmake --build MicroErrorSystem/cmake-build-debug --parallel
MicroErrorSystem/cmake-build-debug/MicroErrorSystemBenchmark.exe
```

Sanitizers на Clang/Ninja:

```text
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/cmake-build-asan -DBUILD_TESTING=ON -DENABLE_SANITIZERS=ON
cmake --build MicroErrorSystem/cmake-build-asan --parallel
ctest --test-dir MicroErrorSystem/cmake-build-asan --output-on-failure
```

Fuzzing на Unix-платформах с Clang:

```text
cmake -G Ninja -S MicroErrorSystem -B MicroErrorSystem/cmake-build-fuzz -DBUILD_FUZZERS=ON
cmake --build MicroErrorSystem/cmake-build-fuzz --parallel
MicroErrorSystem/cmake-build-fuzz/MicroErrorSystemFuzzer -runs=10000
```

Интеграционный стресс-тест с third-party workload:

```text
cmake -S MicroErrorSystem -B MicroErrorSystem/cmake-build-external -DBUILD_TESTING=ON -DBUILD_EXTERNAL_STRESS_TESTS=ON
cmake --build MicroErrorSystem/cmake-build-external --parallel
ctest --test-dir MicroErrorSystem/cmake-build-external -R NlohmannStressTests --output-on-failure
```

Тест фиксирует `nlohmann/json` на `v3.12.0`, не добавляет его в runtime API и
использует его parser как внешний workload для проверки error-контуров.

## Запуск тестов

```text
ctest --test-dir MicroErrorSystem/cmake-build-debug --output-on-failure
```

Тесты проверяют:

- геттеры и сравнение `Error`;
- наличие категории;
- успешные и ошибочные `Result<T>`;
- маршрутизацию `Handler`;
- добавление и удаление ошибок в регистрах.
- лимиты worker pool, backpressure и отмену queued-задач;
- фильтрацию logger policy.

## Быстрый пример

```cpp
#include "vosp_error.hpp"

using namespace vosp::error;

class NetworkRegister final : public CategoryRegister<Category::NETWORK>
{
public:
    OperationResult add(const Error& error) override;
    OperationResult remove(const Error& error) override;
};

NetworkRegister network;
Handler handler{network};

const Error error{Category::NETWORK, 100, "Connection failed"};
const OperationResult registered = handler.add(error);
```

`Handler` не владеет регистрами. Все переданные регистры должны жить дольше
объекта `Handler`. Для одной категории следует передавать только один регистр;
если регистров несколько, используется первый совпавший.

## Структура

```text
MicroErrorSystem/
├── include/vosp_error.hpp       # публичный header-only API
├── tests/vosp_error_tests.cpp   # функциональные тесты
├── CMakeLists.txt               # сборка и CTest
└── docs/ARCHITECTURE.md        # архитектурные ограничения и точки расширения
```

## Расширение

Для добавления категории:

1. добавить значение в `Category`;
2. создать класс-наследник `CategoryRegister<категория>`;
3. реализовать `add` и `remove`;
4. передать регистр в `Handler`;
5. добавить тест маршрутизации.

Подробнее ограничения владения, жизненного цикла и потокобезопасности описаны
в [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
