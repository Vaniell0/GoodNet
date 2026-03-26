# SignalBus

Событийная шина для доставки пакетов к [handlers](../guides/handler-guide.md). Реализация: `include/signals.hpp`, `src/signals.cpp`.

См. также: [Обзор архитектуры](../architecture.md) · [Handler: гайд](../guides/handler-guide.md) · [ConnectionManager](../architecture/connection-manager.md)

## PipelineSignal

Каждый msg_type имеет свой `PipelineSignal` — список handlers, отсортированных по priority (**255 = highest**, вызывается первым; **0 = lowest**, вызывается последним). Сортировка **по убыванию** (`a.priority > b.priority`). Есть wildcard-подписка для handlers, которые хотят видеть все типы сообщений.

```
dispatch_packet() → bus_.dispatch_packet(type, header, endpoint, data)
  │
  ├─ PipelineSignal[type]:
  │   handler_1 (priority=255) → CONTINUE      ← вызывается первым
  │   handler_2 (priority=128) → CONSUMED       ← цепочка остановлена
  │   handler_3 (priority=50)  ← не вызывается
  │
  └─ Wildcard handlers (вызываются для всех типов)
```

### Priority

Priority определяет порядок вызова: **255 = highest** (вызывается первым), **0 = lowest** (вызывается последним). Сортировка по убыванию (`std::stable_sort` с `a.priority > b.priority` в `src/signals.cpp`). Задаётся через `plugin_info_t::priority`.

**Tie-breaking rule (handlers с одинаковым priority):**

Если два handler зарегистрированы с одинаковым priority, они вызываются в **insertion order** (порядке регистрации в PluginManager):

```cpp
// Пример: оба handler с priority=128
register_handler("logger", priority=128)   // регистрируется первым
register_handler("metrics", priority=128)  // регистрируется вторым

// Порядок вызова:
dispatch_packet(type=100, ...) →
  1. logger.handle_message()   ← первый (insertion order)
  2. metrics.handle_message()  ← второй
```

**Проблема:** Insertion order зависит от порядка загрузки плагинов из `base_dir` (filesystem readdir → недетерминирован). Если logger и metrics в разных `.so` файлах, порядок может отличаться между запусками.

**Рекомендация:**
- Используйте **gaps** между priority (50, 100, 150, 200, 250) вместо одинаковых значений
- Если порядок критичен, укажите явно: logger=200, metrics=150 (logger вызывается первым, т.к. priority выше)
- Wildcard handlers всегда вызываются последними (после type-specific)

### Propagation enum

Результат, возвращаемый handler из `handle_message()`, определяет дальнейшую обработку пакета:

| Значение | Числовое | Описание |
|----------|----------|----------|
| `PROPAGATION_CONTINUE` | 0 | Передать пакет следующему handler в цепочке |
| `PROPAGATION_CONSUMED` | 1 | Handler забрал пакет — цепочка останавливается, включается session affinity |
| `PROPAGATION_REJECT` | 2 | Тихо отбросить пакет — цепочка останавливается, пакет не обрабатывается |

Определены в `sdk/types.h`. И `CONSUMED`, и `REJECT` останавливают цепочку, но отличаются семантически: `CONSUMED` означает успешную обработку (пинит affinity), `REJECT` — пакет некорректен или нежелателен (инкрементирует `stats.rejected`).

### Session affinity

`PROPAGATION_CONSUMED` пинит «session affinity» — последующие пакеты на этом соединении идут сразу к этому handler, минуя остальных. Это позволяет handler-у «захватить» connection для обработки протокола.

**Performance implications:**

| Без affinity | С affinity (CONSUMED) |
|--------------|-----------------------|
| Полный chain traversal (все handlers checked) | Прямой вызов affinity handler (skip chain) |
| ~100-200 ns overhead (priority sort + callbacks) | ~5-10 ns (direct function pointer) |
| O(N) где N = количество handlers | O(1) lookup в affinity map |

**Пример (измерения на тестовом стенде):**
```
100 000 пакетов на одном connection:

Без affinity (3 handlers в chain):
  dispatch_packet(): 18 ms total
  Avg per packet: 180 ns

С affinity (CONSUMED первым пакетом):
  dispatch_packet(): 0.6 ms total
  Avg per packet: 6 ns

Gain: ~30x speed improvement
```

**Gotcha — stuck connection:**

Если handler с affinity **crashes или unload**, connection остаётся привязанным к мёртвому handler:

```cpp
// Время  Операция                     Состояние
   t0    handler->handle_message()    → CONSUMED (affinity pinned)
   t1    pm->unload_plugin("handler") → handler deleted
   t2    Новый пакет на том же conn   → affinity map указывает на nullptr
                                       → SEGFAULT или drop packet
```

**Защита (текущая реализация):**
- PluginManager при unload вызывает `SignalBus::unsubscribe(sub_id)` для всех подписок handler
- `unsubscribe()` вызывает `PipelineSignal::disconnect(name)` — удаляет handler из цепочки
- `disconnect()` пересобирает вектор handlers (RCU: копия без удалённого handler → atomic store)
- Следующий пакет → full chain traversal (как будто affinity не было)

### Методы удаления подписок

**`PipelineSignal::disconnect(name)`** — удаляет handler по имени из конкретного канала (per-type или wildcard). Пересобирает отсортированный вектор без удалённого handler. Thread-safe (mutex на write path).

**`SignalBus::unsubscribe(sub_id)`** — удаляет подписку по ID (возвращённому из `subscribe()`). Внутри находит msg_type и name через `sub_map_`, затем вызывает `disconnect()` на соответствующем канале.

**Рекомендация:**
- Используйте CONSUMED только для stateful protocols (chat sessions, file transfers)
- Для stateless handlers (metrics, logger) используйте CONTINUE
- При unload плагина с affinity: сначала graceful disconnect всех pinned connections, затем unload

## EventSignal

`EventSignal<Args...>` — асинхронный broadcast событий через `io_context` strand. Используется для уведомлений о состоянии соединений (`on_conn_state`), не для hot path.

Strand гарантирует сериализацию: callbacks не вызываются параллельно, даже при множестве IO-потоков.

## Stats

Atomic counters для мониторинга (lock-free):

- `rx_bytes`, `tx_bytes` — трафик
- `rx_packets`, `tx_packets` — счётчик пакетов
- `auth_ok`, `auth_fail` — аутентификация
- `decrypt_fail` — ошибки дешифрации
- `backpressure` — срабатывания backpressure

`StatsSnapshot` собирает текущее состояние счётчиков. Dashboard в benchmark binary обновляется с заданной частотой (`--hz`).

### DropReason

Enum перечисляющий причины отброса пакетов:

- `BadMagic` — некорректный magic в header
- `BadProtoVer` — неподдерживаемая версия протокола
- `DecryptFail` — ошибка AEAD дешифрации
- `ReplayDetected` — нарушение монотонности nonce
- `RecvBufOverflow` — recv_buf превысил MAX_RECV_BUF (16 MB)
- `ConnectorNotFound` — connector выгружен (TOCTOU)
- `Backpressure` — превышен лимит pending bytes
- и другие

### LatencyHistogram

Lock-free гистограмма задержек с 7 экспоненциальными бакетами. Используется для отображения распределения latency в dashboard. Реализация: `include/signals.hpp` (`struct LatencyHistogram`).

**Бакеты (exponential, наносекунды):**

| Индекс | Порог | Человекочитаемо |
|--------|-------|-----------------|
| 0 | < 1 000 ns | < 1 us |
| 1 | < 10 000 ns | < 10 us |
| 2 | < 100 000 ns | < 100 us |
| 3 | < 1 000 000 ns | < 1 ms |
| 4 | < 10 000 000 ns | < 10 ms |
| 5 | < 100 000 000 ns | < 100 ms |
| 6 | UINT64_MAX | >= 100 ms (catch-all) |

Все операции атомарные (`std::memory_order_relaxed`). `record(ns)` — инкрементирует соответствующий бакет + `total_ns` + `count`. `avg_ns()` — средняя задержка (total_ns / count).

Гистограмма копируется в `StatsSnapshot::dispatch_latency` при вызове `stats_snapshot()` (approximate, non-atomic read — достаточно для телеметрии).

---

**См. также:** [Обзор архитектуры](../architecture.md) · [Handler: гайд](../guides/handler-guide.md) · [ConnectionManager](../architecture/connection-manager.md)
