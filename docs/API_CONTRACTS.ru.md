# Контракты публичного API

## Статус совместимости

Текущая линия — `0.2.x-beta`. Публичным считается API, доступный через
`include/vosp.hpp`; объявления в `detail` не являются контрактом. До 1.0 minor
release может менять source compatibility с записью в `CHANGELOG.md`. Patch
release сохраняет source compatibility: он может добавлять opt-in API и
проверки, но не удаляет объявления и не меняет документированную семантику.

## Стабильные source-контракты 0.2.x

- `<vosp.hpp>` — единая точка подключения в source tree и installed package.
- `Error` владеет сообщением и сравнивает category, code и message.
  `category_name()` возвращает стабильное имя в верхнем регистре, а
  `to_string(error)` — строку `[CATEGORY:code] message`.
- `std::formatter<Error>` выдаёт тот же результат, что и `to_string()`.
  Поддерживается только пустая спецификация `{}`; остальные отклоняются через
  `std::format_error`.
- `Result<T>` является `std::expected<T, Error>`, а `OperationResult` —
  `Result<void>`.
- `Register`, `System`, `Sink` и `Logger` — основные policy-selected типы.
  Compatibility aliases не добавляют storage, allocation, virtual dispatch или
  runtime mode selection.
- Policies выбираются на этапе компиляции и ограничены concepts. Неподходящие
  register, executor, sink и logger policies намеренно не компилируются.
- Benchmarks и third-party integrations являются opt-in инструментами
  репозитория и не входят в installed package.

## Проверка контрактов

- positive tests собирают и выполняют поддерживаемый API;
- negative runtime tests проверяют formatter, routing, sink и queue failures;
- compile-fail targets подтверждают отклонение неподходящих policies;
- package consumer собирается только по установленным headers и CMake config.

CTest использует label `api-contract`, а compile-fail cases дополнительно
помечены `compile-fail`. Эти проверки запускаются отдельным GitHub Actions gate.

## Владение и время жизни

- `Error` владеет message; `message()` возвращает view до изменения/уничтожения
  конкретного `Error`.
- `Register` владеет сохранёнными errors.
- `System` и handlers хранят ссылки на registers.
- Async `System` хранит ссылку на executor; executor и registers должны жить
  дольше всех submitted operations.
- Sink, подключённый по ссылке, должен жить дольше logger и in-flight callbacks.
- Sink, подключённый через `shared_ptr`, удерживается logger и delivery snapshot.
- `WorkerPool` владеет queue callbacks и worker threads.

## Потокобезопасность

| Тип/режим | Контракт |
| --- | --- |
| default `Register` | внутренний mutex |
| `Register<..., SingleThreaded>` | только внешний serialized access |
| single-threaded `System` | не добавляет handler locks |
| multi-threaded `System` | отдельный routing lock на register |
| serialized `Logger` | sink callbacks выполняются последовательно |
| parallel `Logger` | sink обязан поддерживать concurrent `write` |
| async `Logger` | один consumer обрабатывает bounded queue |
| immediate `Sink` | stream output сериализован |
| buffered `Sink` | producer-local buffers и serialized stream output |
| `WorkerPool` | submission, wait, clear и shutdown thread-safe |

## WorkerPool

- worker count и queue capacity находятся в `[1, 1024]`;
- нулевой worker count выбирает `hardware_concurrency()`;
- submit в полной queue блокируется до свободного slot или shutdown;
- submit после shutdown выбрасывает `std::runtime_error`;
- `submit()` возвращает future, `dispatch()` учитывает ошибки в counters;
- `clear_queue()` отменяет waiting tasks, но не active tasks;
- `DRAIN` выполняет принятые задачи и не запрашивает stop;
- `CANCEL_PENDING` отменяет queue и запрашивает cooperative stop;
- worker не выполняет self-join; финальный join остаётся у owner.

## Logger

- synchronous `write()` возвращает true, только если все sink приняли record;
- async `write()` сообщает о принятии в queue, не о финальном sink result;
- async sink failures и exceptions учитываются в `failed_records()`;
- async `flush()` ждёт delivery callbacks, но не вызывает buffered sink flush;
- async queue bounded до 1 024 records и использует blocking backpressure;
- publish после shutdown возвращает false;
- `detach()` не является lifetime barrier для уже выбранного callback.

## Ошибки

Domain failures возвращаются через `std::expected`. Нарушение constructor
arguments, allocation failure, submit после shutdown и exceptions из user
callbacks используют C++ exceptions согласно контракту конкретной операции.

## Ограничения

- зависший sink может задержать caller, flush или shutdown;
- running callback нельзя принудительно завершить безопасно;
- file rotation, fsync, quota и retention остаются ответственностью приложения;
- benchmark null streams не измеряют реальную latency storage.
