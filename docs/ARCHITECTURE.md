# Архитектура MicroErrorSystem

## Область ответственности

Модуль описывает значение ошибки, категории ошибок и диспетчеризацию ошибок
в специализированные регистры. Хранение регистров, синхронизация потоков и
персистентность находятся за пределами текущей заготовки.

## Компоненты

### `Error`

Небольшой объект значения с кодом, сообщением и категорией. Объект владеет
текстом сообщения, поэтому передача временной строки безопасна.

### `IRegister`

Абстрактный интерфейс регистра одной категории. Реализация отвечает за свою
политику хранения, проверку дубликатов и удаление ошибок.

### `MemoryRegister`

Готовая потокобезопасная реализация регистра на `std::unordered_set`. Она
предоставляет среднюю O(1) сложность поиска, добавления и удаления и подходит
как базовое in-memory-хранилище.
Регистр ограничен параметром `capacity_limit` и жёстким пределом
`max_register_capacity`; после достижения лимита новая ошибка отклоняется с
`register_capacity_error_code`. Ошибка другой категории также отклоняется.

### `Handler`

Невладеющий маршрутизатор. Он последовательно проверяет категории переданных
регистров и вызывает `add` или `remove` у первого совпавшего регистра.

### `ErrorSystem`

Внутренний `ErrorSystem<TypeRegister, TypeHandler, ...>` выбирает режим работы
на этапе компиляции. Пользовательский API использует короткие псевдонимы:

- `SingleThreadedSystem<Registers...>`;
- `MultiThreadedSystem<Registers...>`;
- `AsyncSystem<Executor, Registers...>`.

Режимы:

- `SingleThreadedRegister` + `SingleThreadedHandler` — без синхронизации;
- `MultiThreadedRegister` + `MultiThreadedHandler` — операции защищены mutex;
- `AsyncRegister<Executor>` + `AsyncHandler<Executor>` — операции передаются
  внешнему executor и возвращают `std::future<OperationResult>`. Async handler
  хранится через `shared_ptr` и захватывается задачей по значению, поэтому
  уничтожение `AsyncSystem` после постановки задачи не использует уничтоженный
  `this`.

### `Result<T>`

Псевдоним `std::expected<T, Error>` для операций, которым нужно вернуть либо
значение, либо описание ошибки.

### `Logger`

`Logger` публикует `LogEntry` в один или несколько невладеющих `ILogSink`.
Регистры и logger разделены: регистр хранит состояние ошибок, logger передаёт
события наблюдаемости. Список sink копируется под mutex, а пользовательские
вызовы sink выполняются после освобождения mutex. Это допускает reentrant
`attach/detach`, но sink по-прежнему должен жить дольше logger.

### `vosp.hpp`

Единая публичная точка подключения, экспортирующая API ошибок и logger.

### `IndustrialWorkerPool`

An owning `std::jthread` executor with a fixed worker count. Tasks use a
preallocated bounded ring queue, so steady-state submission does not allocate
queue nodes. `DRAIN` processes accepted work before workers exit, while
`CANCEL_PENDING` resolves queued futures with a cancellation error and requests
cooperative cancellation from active callbacks. Worker-initiated shutdown only
signals the transition; the owner performs the final joins and cannot self-join.
State is observable through `pending_tasks()`, `active_tasks()`, and
`is_stopping()`. A task item stores exactly one ordinary or cancellable callback,
avoiding nested `std::function` wrappers. Tracked `submit()` creates a future;
fire-and-forget `dispatch()` omits that shared state and exposes aggregate
failure/cancellation counters. `dispatch_bulk()` amortizes producer-side
locking across grouped ring-buffer refills while workers still claim one task
at a time, preserving precise `clear_queue()` cancellation semantics.

## Поток обработки

```text
Error
  │
  ▼
Handler::add/remove
  │ сравнение Category
  ▼
IRegister соответствующей категории
  │
  ▼
OperationResult или std::future<OperationResult>
```

## Инварианты

- `Handler` не владеет регистрами;
- регистры должны жить дольше `Handler`;
- у одного `Handler` не должно быть нескольких регистров одной категории;
- `Category::NONE` не маршрутизируется в специализированный регистр;
- однопоточный режим не обеспечивает потокобезопасность;
- многопоточный режим синхронизирует операции через `ErrorSystem`;
- асинхронный режим не владеет executor, он должен жить дольше всех futures;
  переданные регистры также должны жить дольше всех async futures;
- `[[nodiscard]]` результаты `add`, `remove` и `Result` нельзя игнорировать.

## Точки расширения

Новые регистры добавляются наследованием от
`CategoryRegister<НоваяКатегория>`. Новый регистр должен реализовать собственную
политику хранения, `add()` и `remove()`.

## Ограничения текущей версии

- при совпадении нескольких регистров используется первый;
- операции регистрации возвращают `OperationResult`, поэтому причина отказа
  передаётся через `Error`;
- асинхронный executor должен принимать `std::function<OperationResult()>` и
  возвращать `std::future<OperationResult>`;
- logger не владеет sink, а `ConsoleSink` и `BufferedStreamSink` не владеют
  переданным `std::ostream`; перед уничтожением потока вывода buffered sink
  требует явного `flush()` после завершения producer-потоков;
- предопределённые ошибки используют `inline const`, поскольку `Error` владеет
  сообщением через `std::string` и не является литеральным `constexpr`-типом.

## Упрощение реализации C++23

- `Handler` и `ConcurrentHandler` сохраняют публичные имена, но используют одно
  внутреннее ядро маршрутизации; синхронизация включается через `if constexpr`;
- две синхронные специализации `ErrorSystem` используют одно общее RAII-ядро;
- `Error::operator!=` синтезируется стандартом из defaulted `operator==`;
- уровни logger используют один constrained forwarding-путь без дублирования
  перегрузок для lvalue/rvalue;
- shards буферизованного sink хранятся непосредственно в `std::deque`, которая
  сохраняет адреса элементов, и выровнены для исключения false sharing;
- четыре конструктора внутренних worker-задач заменены одним constrained
  конструктором; явная ветка исполнения двух callback-типов оставлена после
  benchmark-проверки как более быстрая на используемом Clang toolchain.

Публичные implementation-заголовки сокращены с 2 184 до 2 072 строк. Это
снижение на 112 строк (5,1%) без изменения документированных пользовательских
вызовов.
