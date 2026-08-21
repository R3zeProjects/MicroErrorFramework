# Миграция на единый API

## Цель

Единый API оставляет одно основное имя на каждую ответственность, а варианты
поведения переносит в compile-time политики. Реализация hot path, владение и
формат ошибок не меняются.

## Соответствие имён

| Предыдущее имя | Единый API |
| --- | --- |
| `MemoryRegister<Category>` | `Register<Category>` |
| `SingleThreadedSystem<...>` | `System<system_policy::SingleThreaded, ...>` |
| `MultiThreadedSystem<...>` | `System<system_policy::MultiThreaded, ...>` |
| `AsyncSystem<Executor, ...>` | `System<system_policy::Async<Executor>, ...>` |
| `ConsoleSink` | `Sink<>` или `Sink sink{stream}` |
| `BufferedStreamSink` | `Sink<sink_policy::Buffered>` |
| `PolicyLogger<...>` | `Logger<...>` |
| `ParallelLogger` | `Logger<AcceptAll, Parallel>` |
| `IndustrialWorkerPool` | `WorkerPool` |

Предыдущие имена остаются compatibility aliases на период beta-миграции.

## Группы политик

- `register_policy` — внутренняя синхронизация register;
- `system_policy` — direct, synchronized или async routing;
- `sink_policy` — immediate или producer-buffered вывод;
- `logger_policy` — фильтрация, dispatch и metadata.

Политики являются stateless-типами и не добавляют runtime mode switch.

## Пример

```cpp
Register<Category::NETWORK> network;
System<system_policy::MultiThreaded, decltype(network)> errors{network};

vosp::logger::Sink output{std::cout};
vosp::logger::Logger logger{output};
```

Для register без внутренней блокировки:

```cpp
using NetworkRegister =
    Register<Category::NETWORK, register_policy::SingleThreaded>;
NetworkRegister network;
System<system_policy::MultiThreaded, NetworkRegister> errors{network};
```

Прямой конкурентный доступ к такому register запрещён: все вызовы должны идти
через синхронизирующий `System`.
