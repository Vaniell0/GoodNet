# ConnectionManager

## Назначение

`ConnectionManager` — центральный компонент ядра GoodNet, который:

- **Владеет реестром соединений** — хранит лёгкие записи `ConnectionRecord` для каждого активного соединения
- **Управляет идентификацией** — загружает и хранит Ed25519 ключи узла (`NodeIdentity`)
- **Авторизует пиров** — проверяет подпись при установке соединения
- **Шифрует трафик** — прозрачно для плагинов и хендлеров
- **Маршрутизирует** — принимает `api_->send(uri, ...)` от хендлеров, находит нужный коннектор по схеме URI и отправляет через него
- **Эмитирует `PacketSignal`** — после расшифровки и верификации пакета сигнал уходит в хендлеры

---

## Архитектура владения

```
Плагин (libtcp.so)           Ядро (goodnet_core.so)
──────────────────────────   ──────────────────────────────────────
IConnection (сокет, буфер)   ConnectionRecord (state, endpoint)
     │                                │
     │  api_->on_connect(ep)          │  генерирует conn_id
     └──────────────────────────────► │ ◄── conn_id возвращается плагину
                                      │
     │  api_->on_data(conn_id, bytes) │  расшифровывает, собирает пакет
     └──────────────────────────────► │ ──► PacketSignal ──► Handlers
                                      │
     ◄────────────────────────────────│  connector_ops_t::send_to(conn_id, bytes)
  do_send(зашифрованные байты)        │
```

**Ключевой принцип:** ядро никогда не хранит указатели на объекты плагина. Плагин никогда не получает указатели на объекты ядра. Всё общение — через `conn_id` (uint64_t) и `host_api_t`.

---

## NodeIdentity — идентификация узла

Каждый узел GoodNet имеет два Ed25519 keypair:

| Ключ | Назначение | Переносимость |
|------|-----------|---------------|
| `user_key` | Аккаунт пользователя. Это "лицо" в сети — по `user_pubkey` вас находят другие узлы. | Переносим: скопируй `~/.goodnet/user_key` на другое устройство |
| `device_key` | Идентификатор конкретной машины. Генерируется один раз. | Не переносим: привязан к устройству |

При регистрации узел подписывает `user_pubkey || device_pubkey` своим `user_seckey`. Это означает: *"я — этот пользователь, и я доверяю этому устройству"*.

### Хранение ключей

```
~/.goodnet/
    user_key    — 64 байта Ed25519 secret key (chmod 600)
    device_key  — 64 байта Ed25519 secret key (chmod 600)
```

При старте: если файл существует — загружается, если нет — генерируется новый.

---

## AUTH flow

Сразу после установки TCP-соединения оба узла обмениваются AUTH-пакетами:

```
A ──────────────────────────────────────────────────► B
  header_t { magic=GNET_MAGIC, type=MSG_TYPE_AUTH }
  auth_payload_t {
      user_pubkey[32]    ← публичный ключ аккаунта
      device_pubkey[32]  ← публичный ключ устройства
      signature[64]      ← Ed25519(user_seckey,
                                   user_pubkey || device_pubkey)
  }

B проверяет:
  1. GNET_MAGIC и proto_ver совпадают → иначе разрыв
  2. verify(signature, user_pubkey) == ok → иначе разрыв
  3. Сохраняет peer_user_pubkey, peer_device_pubkey
  4. Добавляет в pk_index → теперь узел A доступен по "gn://user_pubkey_hex"
  5. STATE → ESTABLISHED
```

---

## Адресация

`ConnectionManager` поддерживает несколько форматов URI для `send()`:

| Формат | Пример | Описание |
|--------|--------|----------|
| `scheme://ip:port` | `tcp://192.168.1.1:8080` | Прямой адрес, выбор коннектора по схеме |
| `gn://pubkey_hex` | `gn://ab12cd...` | По публичному ключу аккаунта (32 байта hex) |
| `alias://name` | `alias://my-node` | По псевдониму (таблица в ConnectionManager, TODO) |
| Кастомный | `myproto://...` | Любая схема — ищет плагин с `get_scheme() == "myproto"` |

Плагин-коннектор объявляет свою схему через `get_scheme()` и не знает о других форматах.

---

## Шифрование

Шифрование прозрачно для плагинов и хендлеров:

- **Плагин** передаёт сырые байты в `api_->on_data()` — без расшифровки
- **ConnectionManager** расшифровывает сессионным ключом (X25519 + XSalsa20-Poly1305)
- **Хендлер** получает чистый plaintext через `PacketSignal`

Приватные ключи никогда не покидают `ConnectionManager`. Плагин может попросить ядро подписать буфер через `api_->sign_with_device()` — но ключ при этом остаётся в ядре.

```
Плагин              ConnectionManager           Хендлер
─────────────────   ─────────────────────────   ────────────
on_data(raw_bytes)
     │
     ▼
  [зашифровано]  ──►  decrypt(session_key)
                       verify(header.signature)
                       emit(PacketSignal)   ──────────────► handle_message(plaintext)
```

### Текущий статус

| Компонент | Статус |
|-----------|--------|
| Ed25519 AUTH (подпись ключей) | ✅ реализовано |
| Сборка TCP-фрагментов | ✅ реализовано |
| X25519 key exchange (сессионный ключ) | 🔧 TODO |
| XSalsa20-Poly1305 шифрование | 🔧 TODO (заглушка — plaintext) |
| Таблица алиасов | 🔧 TODO |

---

## API для плагинов

Плагин общается с ядром **только** через `host_api_t`:

```c
typedef struct {
    // Коллбэки коннектора → ядро (обязательно вызывать при событиях)
    conn_id_t (*on_connect)   (void* ctx, const endpoint_t* ep);
    void      (*on_data)      (void* ctx, conn_id_t id, const void* raw, size_t n);
    void      (*on_disconnect)(void* ctx, conn_id_t id, int error_code);

    // Хендлер просит ядро отправить данные
    void (*send)(void* ctx, const char* uri, uint32_t type,
                 const void* payload, size_t size);

    // Крипто: плагин просит — ядро делает
    int (*sign_with_device) (void* ctx, const void* data, size_t n, uint8_t sig[64]);
    int (*verify_signature) (void* ctx, const void* data, size_t n,
                             const uint8_t* pubkey, const uint8_t* sig);

    void* ctx;  // непрозрачный контекст — передавать первым аргументом
} host_api_t;
```

---

## Поток данных

### Входящее сообщение

```
TCP socket
    │ async_read
    ▼
TcpConnection::do_read()
    │ notify_data(raw, n)
    ▼
api_->on_data(ctx, conn_id, raw, n)
    │
    ▼
ConnectionManager::handle_data()
    │ append to recv_buf
    │ check header_t.magic
    │ assemble complete packet (TCP fragmentation)
    │
    ├─ MSG_TYPE_AUTH     → process_auth() → STATE_ESTABLISHED
    ├─ MSG_TYPE_KEY_EXCHANGE → TODO
    │
    └─ STATE_ESTABLISHED → decrypt() → PacketSignal::emit()
                                           │
                                           ▼
                                     handler_t::handle_message()
```

### Исходящее сообщение

```
IHandler::send(uri, type, payload)
    │ api_->send(ctx, uri, type, payload, size)
    ▼
ConnectionManager::send()
    │ resolve_uri(uri) → conn_id
    │   ├─ uri_index["host:port"] → conn_id
    │   ├─ pk_index["pubkey_hex"] → conn_id  (gn:// адресация)
    │   └─ нет соединения → connector_ops_t::connect(uri)
    │
    │ build header_t + payload
    │ TODO: encrypt(session_key)
    ▼
connector_ops_t::send_to(conn_id, wire_bytes)
    │
    ▼
IConnector → connections_[conn_id]::do_send()
    │
    ▼
TCP socket write
```

---

## Добавление нового коннектора

Коннектор регистрируется в `ConnectionManager` после загрузки плагина:

```cpp
// main.cpp — после plugin_mgr.load_all_plugins()
auto opt = plugin_mgr.find_connector_by_scheme("myproto");
if (opt) conn_mgr.register_connector("myproto", *opt);
```

После этого любой вызов `api_->send("myproto://...", ...)` будет маршрутизирован в этот коннектор.