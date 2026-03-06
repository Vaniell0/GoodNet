# 04 — Криптография

Все примитивы — **libsodium**. Никаких самописных алгоритмов.

---

## Таблица примитивов

| Операция | Примитив libsodium | Ключ/вывод |
|---|---|---|
| Подпись AUTH, заголовков | `crypto_sign_ed25519` | sk: 64 B, pk: 32 B, sig: 64 B |
| ECDH обмен ключами | `crypto_scalarmult` (X25519) | shared: 32 B |
| KDF поверх ECDH | `crypto_generichash` (BLAKE2b-256) | 32 B |
| Шифрование пакетов | `crypto_secretbox_easy` (XSalsa20-Poly1305) | MAC: 16 B |
| Хэш hardware ID | `crypto_generichash` (BLAKE2b-256) | 32 B |
| Seed device_key | `crypto_generichash` (BLAKE2b-256) | 32 B |
| SHA-256 манифестов | `crypto_hash_sha256` | 32 B |
| Base64 → binary | `sodium_base642bin` | — |
| Безопасное затирание | `sodium_memzero` | — |
| Случайные байты | `randombytes_buf` | — |

---

## Ключевые пары

### user_key (Ed25519)

Идентификатор пользователя. Переносимый. Может быть SSH-ключом.

```
Источники (приоритет):
  1. config: "identity.ssh_key_path"
  2. ~/.ssh/id_ed25519  (HOME / USERPROFILE на Windows)
  3. Генерация → <dir>/user_key  (64 байта raw, chmod 0600)

Использование:
  • Подпись AUTH пакетов          (user_seckey)
  • Идентификация в endpoint_t    (user_pubkey)
  • Domain separator в device KDF (user_pubkey)
```

### device_key (Ed25519)

Привязан к конкретному железу. Детерминированно восстанавливается.

```
seed       = BLAKE2b-256(machine_id ‖ user_pubkey)
device_key = crypto_sign_seed_keypair(seed)
sodium_memzero(seed)

Использование:
  • Подпись заголовков в ESTABLISHED   (device_seckey)
  • api->sign_with_device() для плагинов
```

Почему два ключа? `user_key` = "кто ты", `device_key` = "с какой машины". Компрометация `device_key` (утекла VM) не раскрывает `user_key`.

### ephem_key (X25519)

Одноразовый. Генерируется при каждом новом соединении.

```cpp
// cm_handshake.cpp: handle_connect()
rec.session = std::make_unique<SessionState>();
crypto_box_keypair(rec.session->my_ephem_pk,
                   rec.session->my_ephem_sk);

// После derive_session():
sess.clear_ephemeral();  // sodium_memzero(sk) + sodium_memzero(pk)
```

---

## Деривация сессионного ключа

```cpp
// cm_session.cpp: derive_session()

// 1. ECDH
uint8_t shared[32];
crypto_scalarmult(shared, sess.my_ephem_sk, peer_ephem_pk);

// 2. Domain separation: сортируем user_pk лексикографически
const uint8_t* pk_a = identity_.user_pubkey;
const uint8_t* pk_b = peer_user_pk;
if (std::memcmp(pk_a, pk_b, 32) > 0) std::swap(pk_a, pk_b);

// 3. KDF: BLAKE2b-256(shared ‖ pk_min ‖ pk_max)
crypto_generichash_state st;
crypto_generichash_init(&st, nullptr, 0, 32);
crypto_generichash_update(&st, shared, 32);
crypto_generichash_update(&st, pk_a,   32);
crypto_generichash_update(&st, pk_b,   32);
crypto_generichash_final(&st, sess.session_key, 32);

// 4. Затираем промежуточные данные
sodium_memzero(shared, sizeof(shared));
sess.clear_ephemeral();
sess.ready = true;
```

---

## Шифрование / расшифровка

### Отправка

```cpp
// SessionState::encrypt(plain, plain_len)
const uint64_t n = send_nonce.fetch_add(1, std::memory_order_relaxed);

uint8_t nonce24[24] = {};
for (int i = 0; i < 8; ++i)
    nonce24[i] = static_cast<uint8_t>(n >> (i * 8));  // little-endian

// wire = nonce_prefix[8] ‖ secretbox(plain)
// secretbox добавляет 16 байт MAC
std::vector<uint8_t> wire(8 + plain_len + crypto_secretbox_MACBYTES);
std::memcpy(wire.data(), nonce24, 8);
crypto_secretbox_easy(wire.data() + 8, plain, plain_len, nonce24, session_key);
return wire;
```

### Приём

```cpp
// SessionState::decrypt(wire, wire_len)
if (wire_len < 8 + crypto_secretbox_MACBYTES) return {};

uint64_t n = 0;
for (int i = 0; i < 8; ++i)
    n |= static_cast<uint64_t>(wire[i]) << (i * 8);

// Replay protection
const uint64_t exp = recv_nonce_expected.load(std::memory_order_acquire);
if (n < exp) {
    LOG_WARN("decrypt: replay nonce={} expected={}", n, exp);
    return {};
}
recv_nonce_expected.store(n + 1, std::memory_order_release);

uint8_t nonce24[24] = {};
std::memcpy(nonce24, wire, 8);

std::vector<uint8_t> plain(wire_len - 8 - crypto_secretbox_MACBYTES);
if (crypto_secretbox_open_easy(plain.data(), wire + 8,
        wire_len - 8, nonce24, session_key) != 0) {
    LOG_WARN("decrypt: MAC failed nonce={}", n);
    return {};
}
return plain;
```

### Overhead

```
8 байт   nonce prefix (uint64_t LE)
16 байт  Poly1305 MAC
─────────
24 байта на каждый зашифрованный пакет
```

---

## OpenSSH Ed25519 parser

`cm_identity.cpp: try_load_ssh_key()`

Поддерживаются только незашифрованные (`cipher = "none"`) Ed25519 ключи формата OpenSSH.

### Структура бинарного блока

```
"openssh-key-v1\0"           magic (15 bytes + NUL)
string  cipher               должно быть "none"
string  kdfname              "none"
blob    kdf_options          пустой
uint32  num_keys             == 1
blob    pubkey_blob          пропускаем
blob    private_block:
  uint32  checkint[2]        должны совпадать (защита от неверного passphrase)
  string  key_type           "ssh-ed25519"
  blob    pub[32]            Ed25519 pubkey
  blob    sec[64]            Ed25519 seckey (seed[32] ‖ pub[32])
  string  comment            пропускаем
```

### Base64 декодирование

```cpp
// cm_identity.cpp
static std::vector<uint8_t> base64_decode(std::string_view in) {
    std::vector<uint8_t> out(in.size()); // верхняя граница: 3/4 от b64
    size_t bin_len = 0;
    if (sodium_base642bin(out.data(), out.size(),
                          in.data(),  in.size(),
                          nullptr,    &bin_len,
                          nullptr,    sodium_base64_VARIANT_ORIGINAL) != 0)
        return {};
    out.resize(bin_len);
    return out;
}
```

`sodium_base64_VARIANT_ORIGINAL` — стандартный RFC 4648 Base64, используется в OpenSSH.
`sodium_base642bin` корректно обрабатывает whitespace (переносы строк в PEM) и padding.

---

## Управление памятью секретов

| Данные | Когда затирается |
|---|---|
| `ephem_sk` | Сразу после `derive_session()` |
| `ephem_pk` | Сразу после `derive_session()` |
| ECDH `shared` | Сразу после BLAKE2b final |
| `device_key seed` | Сразу после `crypto_sign_seed_keypair()` |
| `session_key` | В деструкторе `~SessionState()` |

`sodium_memzero` гарантирует, что компилятор не оптимизирует затирание (в отличие от `memset`, который может быть выброшен как dead store).

---

## Ограничения (alpha)

**Нет PFS в строгом смысле.** Если `user_seckey` компрометирован задним числом + записан весь трафик → можно восстановить `session_key` из перехваченного AUTH. Настоящий PFS требует ephemeral signing (подпись ephem_pk краткосрочным ключом) — выходит за рамки alpha.

**Нет double ratchet.** Сессионный ключ фиксирован на время соединения. Компрометация `session_key` раскрывает всю сессию, не только один пакет.

---

*← [03 — Протокол](03-protocol.md) · [05 — ConnectionManager →](05-connection-manager.md)*
