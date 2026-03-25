# SignalBus

Событийная шина для доставки пакетов к [handlers](../guides/handler-guide.md). Реализация: `include/signals.hpp`, `src/signals.cpp`.

См. также: [Обзор архитектуры](data/projects/GoodNet/docs/architecture.md) · [Handler: гайд](../guides/handler-guide.md) · [ConnectionManager](data/projects/GoodNet/docs/architecture/connection-manager.md)

## PipelineSignal

Каждый msg_type имеет свой `PipelineSignal` — список handlers, отсортированных по priority (**0 = highest**, 255 = lowest). Есть wildcard-подписка для handlers, которые хотят видеть все типы сообщений.

```
dispatch_packet() → bus_.dispatch_packet(type, header, endpoint, data)
  │
  ├─ PipelineSignal[type]:
  │   handler_1 (priority=0)   → CONTINUE      ← вызывается первым
  │   handler_2 (priority=128) → CONSUMED       ← цепочка остановлена
  │   handler_3 (priority=200) ← не вызывается
  │
  └─ Wildcard handlers (вызываются для всех типов)
```

### Priority

Priority определяет порядок вызова: **0 = highest** (вызывается первым), **255 = lowest** (вызывается последним). Задаётся через `plugin_info_t::priority`.

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
- Используйте **gaps** между priority (0, 50, 100, 150, 200) вместо одинаковых значений
- Если порядок критичен, укажите явно: logger=100, metrics=150
- Wildcard handlers всегда вызываются последними (после type-specific)

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
- PluginManager при unload вызывает `SignalBus::remove_handler()`
- `remove_handler()` очищает affinity для всех connections этого handler
- Следующий пакет → full chain traversal (как будто affinity не было)

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

Lock-free гистограмма задержек с 7 экспоненциальными бакетами. Используется для отображения распределения latency в dashboard.

---

**См. также:** [Обзор архитектуры](data/projects/GoodNet/docs/architecture.md) · [Handler: гайд](../guides/handler-guide.md) · [ConnectionManager](data/projects/GoodNet/docs/architecture/connection-manager.md)
