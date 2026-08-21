# Архитектура MicroErrorSystem

## Область ответственности

MicroErrorSystem создаёт независимый контур ошибок, логгирования и выполнения
задач. Фреймворк не реализует recovery бизнес-логики, persistent storage,
log rotation или distributed transport.

## Компоненты

### Error и Result

`Error` владеет категорией, числовым кодом и сообщением. `Result<T>` является
`std::expected<T, Error>`, а `OperationResult` используется для операций без
возвращаемого значения.

### Register и System

`Register<Category, Policy>` хранит ошибки одной категории в bounded
`std::unordered_set`. Default policy защищает storage mutex-ом. Политика
`SingleThreaded` убирает внутренний lock для внешне сериализованного доступа.

`System<Policy, Registers...>` маршрутизирует ошибку по категории:

- `system_policy::SingleThreaded` — direct routing;
- `system_policy::MultiThreaded` — отдельный mutex на каждый register;
- `system_policy::Async<Executor>` — owned task через внешний executor.

System не владеет registers. Async system также не владеет executor.

### Logger и Sink

`Logger<Filter, Dispatch, Metadata>` публикует `LogEntry` в один или несколько
`ILogSink`. Policies выбирают level filter, serialized/parallel/async dispatch
и объём metadata.

`Sink<sink_policy::Immediate>` сразу пишет в stream. Buffered policy хранит
отдельный буфер на producer и синхронизирует только вывод в общий stream.
Sink может передаваться logger по ссылке или через `std::shared_ptr`.

### WorkerPool

`WorkerPool` владеет фиксированным набором `std::jthread` и preallocated ring
queue. Максимум — 1 024 workers и 1 024 queued tasks. Полная очередь применяет
blocking backpressure.

## Потоки управления

```text
Error -> System -> category routing -> Register -> OperationResult
Error -> Logger -> optional queue -> Sink
System<Async> -> external executor (например WorkerPool)
```

## Инварианты

- queued errors и log records владеют текстом;
- System и Handler не владеют registers;
- reference sink должен пережить все callbacks, которые могут его вызвать;
- parallel logger требует thread-safe sink;
- buffered sink требует явного `flush()` для проверки delivery status;
- running task нельзя безопасно остановить принудительно;
- worker-initiated shutdown только сигнализирует остановку, owner выполняет join.

## Точки расширения

- новая `Category` и custom `CategoryRegister`;
- executor, удовлетворяющий `AsyncExecutor`;
- собственный `ILogSink`;
- custom logger policies, удовлетворяющие публичным concepts.

Точные правила времени жизни и shutdown описаны в
[контрактах API](API_CONTRACTS.ru.md).
