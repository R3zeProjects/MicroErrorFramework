# Аудит MESLS R1.2

Для оптимизации `0.3.0-beta` изучен предоставленный локальный snapshot
`MESLS-R1.2`. В snapshot отсутствовали Git metadata и upstream URL, а публичный
репозиторий по точному названию не найден, поэтому документация не создаёт
непроверяемую внешнюю ссылку.

Из MESLS оценивались pre-reserved contiguous storage, `std::to_chars`, короткие
critical sections и ring-buffer подходы. В async logger применены две заранее
зарезервированные vector-очереди с O(1) swap. На нагрузке 200 000 records объём
наблюдаемых аллокаций снизился с 33 860 624 до 27 307 808 bytes (−19,4%).

MESLS собран Clang 22.1.6 в C++23 Release: штатная конфигурация прошла 10/10
CTest. Режим `MESLS_WERROR=ON` выявил дефект его API: явно default move
constructor и assignment у `Logger` фактически удалены из-за немобильного
`std::shared_mutex`. Чужой snapshot не изменялся, этот declaration pattern в
MicroErrorFramework не переносился.

Также отклонены возврат registry reference после снятия lock, общий prefetch
задач, нарушающий `clear_queue()`, и spin lock, ухудшивший медианный результат.
Финальная bulk-оптимизация ограничена explicit bulk API и группой до 16 задач.
