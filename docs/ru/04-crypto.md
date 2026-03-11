# 04 — Криптография

Все примитивы — **libsodium**. Никаких самописных алгоритмов.

---

## Таблица примитивов

| Операция | Примитив libsodium | Ключ/вывод |
|---|---|---|
| Подпись AUTH, заголовков | `crypto_sign_ed25519` | sk: 64 B, pk: 32 B, sig: 64 B |
| ECDH обмен ключами | `crypto_scalarmult` (X25519) | shared: 32 B |
| KDF поверх ECDH | `crypto_generichash` (BLAKE2b-256) | 32 B |
| Шифрование пакетов | `crypto_secretbox_easy` (**XSalsa20-Poly1305**) | MAC: 16 B |
| Сжатие пакетов (>512 B) | Zstd level 3 | — |
| Хэш hardware ID | `crypto_generichash` (BLAKE2b-256) | 32 B |
| Seed device_key | `crypto_generichash` (BLAKE2b-256) | 32 B |
| SHA-256 манифестов | `crypto_hash_sha256` | 32 B |
| Base64 → binary | `sodium_base642bin` | — |
| Безопасное затирание | `sodium_memzero` | — |
| Случайные байты | `randombytes_buf` | — |

**Важно:** `crypto_secretbox_easy` — это **XSalsa20-Poly1305**, не ChaCha20-Poly1305. Libsodium использует XSalsa20 (расширенный nonce 192 бит) для `secretbox`. Это стандартный дефолт, не настраивается.

---

## Ключевые пары

### user_key (Ed25519)

Идентификатор пользователя. Переносимый. Может быть SSH-ключом.

```
Источники (приоритет):
  1. cfg.identity.ssh_key_path
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
sess.clear_ephemeral();  // sodium_memzero(sk + pk)
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

**Почему сортировать ключи?** Без сортировки A → `BLAKE2b(shared ‖ A‖B)`, B → `BLAKE2b(shared ‖ B‖A)`. Разные хэши — сессия не установится. Сортировка делает результат детерминированным независимо от направления.

---

## Шифрование / расшифровка

### Отправка

```cpp
// SessionState::encrypt(plain, plain_len)
const uint64_t n = send_nonce.fetch_add(1, std::memory_order_relaxed);

uint8_t nonce24[24] = {};
for (int i = 0; i < 8; ++i)
    nonce24[i] = static_cast<uint8_t>(n >> (i * 8));  // little-endian

// ── Малые пакеты (≤ 512 байт) — без сжатия ─────────────────────────────────
if (plain_len <= 512) {
    std::vector<uint8_t> wire(8 + plain_len + crypto_secretbox_MACBYTES);
    std::memcpy(wire.data(), nonce24, 8);
    crypto_secretbox_easy(wire.data() + 8, plain, plain_len, nonce24, session_key);
    return wire;
}

// ── Большие пакеты (> 512 байт) — Zstd + encrypt ───────────────────────────
uint32_t orig_size = static_cast<uint32_t>(plain_len);
std::vector<uint8_t> compressed(4 + ZSTD_compressBound(plain_len));
std::memcpy(compressed.data(), &orig_size, 4);

size_t csize = ZSTD_compress(compressed.data() + 4, ..., plain, plain_len, 3);
if (ZSTD_isError(csize)) {
    // fallback: без сжатия, но с orig_size префиксом
}
compressed.resize(4 + csize);

std::vector<uint8_t> wire(8 + compressed.size() + crypto_secretbox_MACBYTES);
std::memcpy(wire.data(), nonce24, 8);
crypto_secretbox_easy(wire.data() + 8, compressed.data(), compressed.size(),
                       nonce24, session_key);
```

### Приём

```cpp
// SessionState::decrypt(wire, wire_len)
if (wire_len < 8 + crypto_secretbox_MACBYTES) return {};

uint64_t n = 0;
for (int i = 0; i < 8; ++i) n |= uint64_t(wire[i]) << (i * 8);

// Replay protection
const uint64_t exp = recv_nonce_expected.load(std::memory_order_acquire);
if (n < exp) { LOG_WARN("replay nonce={} expected={}", n, exp); return {}; }
recv_nonce_expected.store(n + 1, std::memory_order_release);

// Decrypt
std::vector<uint8_t> decrypted(wire_len - 8 - crypto_secretbox_MACBYTES);
if (crypto_secretbox_open_easy(decrypted.data(), wire + 8,
        wire_len - 8, nonce24, session_key) != 0) return {};  // MAC fail

// Decompress (если нужно)
if (decrypted.size() <= 512) return decrypted;  // малый пакет

uint32_t orig_size;
std::memcpy(&orig_size, decrypted.data(), 4);
if (orig_size == 0 || orig_size > 100 * 1024 * 1024) {
    return {decrypted.begin() + 4, decrypted.end()};  // нет сжатия
}

std::vector<uint8_t> plain(orig_size);
size_t dsize = ZSTD_decompress(plain.data(), orig_size,
                                decrypted.data() + 4, decrypted.size() - 4);
if (ZSTD_isError(dsize)) {
    return {decrypted.begin() + 4, decrypted.end()};  // fallback
}
return plain;
```

### Overhead на пакет

```
8 байт   nonce prefix (uint64_t LE)
16 байт  Poly1305 MAC
─────────
24 байта постоянный overhead

+ для пакетов > 512 байт:
4 байта  orig_size prefix (внутри зашифрованного блока)
```

---

## OpenSSH Ed25519 parser

`cm_identity.cpp: try_load_ssh_key()` — поддерживаются незашифрованные (`cipher = "none"`) Ed25519 ключи формата OpenSSH.

```cpp
// Base64 декодирование с игнорированием whitespace:
sodium_base642bin(out, out_size, in, in_size,
                  "\n\r ", &bin_len, nullptr,
                  sodium_base64_VARIANT_ORIGINAL);
```

---

## Ротация ключей идентификации

```cpp
// ConnectionManager::rotate_identity_keys(cfg)
// Без остановки сервера. Новые соединения получают новые ключи.
// Существующие сессии продолжают работу со старым session_key.
NodeIdentity next = NodeIdentity::load_or_generate(cfg);
{
    std::unique_lock lk(identity_mu_);
    identity_ = std::move(next);
}
LOG_INFO("Identity keys rotated");
```

---

## Управление памятью секретов

| Данные | Когда затирается |
|---|---|
| `ephem_sk` | Сразу после `derive_session()` |
| `ephem_pk` | Сразу после `derive_session()` |
| ECDH `shared` | Сразу после BLAKE2b final |
| `device_key seed` | Сразу после `crypto_sign_seed_keypair()` |
| `session_key` | В деструкторе `~SessionState()` |

`sodium_memzero` гарантирует, что компилятор не выбросит затирание как dead store.

---

## Ограничения (alpha)

**Нет PFS в строгом смысле.** Если `user_seckey` компрометирован задним числом + записан весь трафик → можно восстановить `session_key` из перехваченного AUTH.

**Нет double ratchet.** Сессионный ключ фиксирован на время соединения.

---

*← [03 — Протокол](03-protocol.md) · [05 — ConnectionManager →](05-connection-manager.md)*