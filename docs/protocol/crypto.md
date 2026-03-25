# Криптография

Вся криптография через **libsodium**. Нет кастомных реализаций. SDK headers не зависят от `sodium.h` — размеры ключей продублированы как `#define` в `sdk/types.h`.

Протокол handshake: **Noise_XX_25519_ChaChaPoly_BLAKE2b** — формализованный 3-message handshake с доказанными свойствами безопасности.

См. также: [Noise_XX handshake](data/projects/GoodNet/docs/protocol/noise-handshake.md) · [Wire format](data/projects/GoodNet/docs/protocol/wire-format.md) · [Конфигурация](data/projects/GoodNet/docs/config.md)

## Ключи

Каждый узел имеет две пары Ed25519 ключей:

### User key (портабельный)

```
user_pubkey  [32 bytes]   — идентификация пользователя в сети
user_seckey  [64 bytes]   — подпись HandshakePayload
```

Хранится в `~/.goodnet/user_key` (или указанный путь). Можно импортировать из OpenSSH Ed25519 (`identity.ssh_key_path` в [конфиге](data/projects/GoodNet/docs/config.md#identitysection)). Переносится между устройствами — это ваш «аккаунт» в сети.

### Device key (привязан к железу)

```
device_pubkey  [32 bytes]   — Ed25519 ключ устройства
device_seckey  [64 bytes]   — Ed25519 secret key
```

Derived из hardware fingerprint:
```
seed = BLAKE2b(machine-id || DMI serial || MAC || CPUID)
device_keypair = Ed25519_from_seed(seed)
```

**Детальный алгоритм деривации:**

```cpp
// Шаг 1: Собрать hardware fingerprint components
machine_id = read_machine_id()  // /etc/machine-id или /var/lib/dbus/machine-id
dmi_serial = read_dmi_serial()  // /sys/class/dmi/id/product_serial (Linux)
mac_addr   = get_primary_mac()  // MAC первого non-loopback интерфейса
cpu_info   = read_cpuid()       // CPUID instruction (x86) или device tree

// Шаг 2: Конкатенация компонентов
fingerprint = machine_id || dmi_serial || mac_addr || cpu_info

// Шаг 3: BLAKE2b hash (256-bit output)
seed[32] = crypto_generichash(fingerprint, fingerprint_len, key=nullptr)

// Шаг 4: Детерминированная генерация Ed25519 keypair
crypto_sign_ed25519_seed_keypair(device_pubkey, device_seckey, seed)

// Шаг 5: Затереть seed из памяти
sodium_memzero(seed, 32)
```

**Особенности:**
- Seed детерминированный → каждый запуск на том же железе даёт одинаковый device key
- Переустановка ОС → `machine-id` изменится → новый device key
- Замена железа (CPU, NIC) → новый device key
- Виртуалки: DMI serial часто одинаковый → device key может совпасть (лучше использовать VM UUID)

Не переносится. Даже если user key скомпрометирован, злоумышленник не может выдать себя за конкретное устройство без физического доступа к железу.

`NodeIdentity::load_or_generate()` (`core/cm_identity.cpp`) — при первом запуске генерирует ключи, при последующих — загружает.

**Device key = Noise static key.** При handshake Ed25519 device key конвертируется в X25519:

```
x25519_static_pk = crypto_sign_ed25519_pk_to_curve25519(device_pubkey)
x25519_static_sk = crypto_sign_ed25519_sk_to_curve25519(device_seckey)
```

X25519-версия device key используется как `s` (static keypair) в Noise [HandshakeState](data/projects/GoodNet/docs/protocol/noise-handshake.md).

### Ephemeral key (генерируется Noise)

Ephemeral keypair генерируется автоматически внутри Noise HandshakeState при выполнении `e` token. Не хранится отдельно — участвует в DH-операциях и смешивается в chaining key. После завершения handshake ephemeral secret key затирается (`sodium_memzero`).

## AEAD шифрование

**Алгоритм**: ChaChaPoly-IETF (`crypto_aead_chacha20poly1305_ietf_encrypt` / `_decrypt`)

### Nonce

12-байтный nonce формируется из `packet_id` [заголовка](data/projects/GoodNet/docs/protocol/wire-format.md#header_t-v3):

```
nonce[12] = 0x00[4] + packet_id_le[8]
```

`packet_id` — монотонный per-connection counter из `header_t`. Nonce **не передаётся** на проводе — получатель берёт packet_id из заголовка.

### Encrypt (`NoiseSession::encrypt`)

```cpp
uint64_t pkt_id = send_packet_id.fetch_add(1);   // atomic, per-connection

uint8_t nonce[12] = {0};
memcpy(nonce + 4, &pkt_id, 8);  // little-endian

// Compression (auto for payload > 512 bytes)
body = compress_if_needed(plain);

// Encrypt
crypto_aead_chacha20poly1305_ietf_encrypt(
    ciphertext, &ct_len,
    body, body_len,
    nullptr, 0,     // no AD
    nullptr, nonce, send_key);

wire = ciphertext;  // no nonce prefix — nonce derived from header
```

### Decrypt (`NoiseSession::decrypt`)

```cpp
uint64_t pkt_id = le64(header->packet_id);

uint8_t nonce[12] = {0};
memcpy(nonce + 4, &pkt_id, 8);

// Replay protection (CAS loop)
uint64_t expected = recv_nonce_expected.load();
while (true) {
    if (pkt_id < expected) return {};  // replay → drop
    if (recv_nonce_expected.compare_exchange_weak(expected, pkt_id + 1))
        break;
}

// Decrypt
crypto_aead_chacha20poly1305_ietf_decrypt(
    body, &body_len, nullptr,
    ciphertext, ct_len,
    nullptr, 0, nonce, recv_key);

// Decompress if needed
```

### Replay protection

Монотонный nonce (из `packet_id` заголовка) + CAS loop. Потокобезопасно без мьютекса:

- `pkt_id < expected` → replay (нарушена монотонность) → drop
- `compare_exchange_weak` атомарно обновляет `expected` → нет TOCTOU

Gap в nonce допускается — nonce только растёт. Strict monotonic counter.

### Replay attack walkthrough

**Сценарий атаки:** Eve перехватывает пакет (packet_id=5, type=100) и пытается повторить отправку.

```
Время  Операция                           Состояние                Результат
─────  ──────────────────────────────────  ───────────────────────  ──────────
  t0   Легитимный пакет (pkt_id=5)        recv_nonce_expected = 5  ✅ accepted
       arrives at Bob
       ├─ Decrypt: CAS(5, 6) успешно      recv_nonce_expected = 6
       └─ dispatch_packet()

  t1   Легитимный пакет (pkt_id=6)        recv_nonce_expected = 6  ✅ accepted
       ├─ Decrypt: CAS(6, 7) успешно      recv_nonce_expected = 7
       └─ dispatch_packet()

  t2   Eve replay пакет (pkt_id=5)        recv_nonce_expected = 7  ❌ REJECTED
       ├─ expected = 7.load()
       ├─ pkt_id < expected (5 < 7)       → replay detected!
       └─ return {} (early exit)          → packet dropped

  t3   Eve пытается replay (pkt_id=6)    recv_nonce_expected = 7  ❌ REJECTED
       ├─ expected = 7.load()
       ├─ pkt_id < expected (6 < 7)       → replay detected!
       └─ return {}                       → packet dropped
```

**Код защиты (из NoiseSession::decrypt):**

```cpp
uint64_t pkt_id = le64(header->packet_id);
uint64_t expected = recv_nonce_expected.load(memory_order_acquire);

while (true) {
    // КРИТИЧНО: проверка ПЕРЕД CAS (предотвращает старые пакеты)
    if (pkt_id < expected) {
        LOG_DEBUG("Replay attack detected: pkt_id={} < expected={}", pkt_id, expected);
        return {};  // drop packet, НЕ decrypt
    }

    // Атомарно обновляем expected → pkt_id + 1
    if (recv_nonce_expected.compare_exchange_weak(expected, pkt_id + 1,
                                                   memory_order_acq_rel)) {
        break;  // Успех, можем decrypt
    }
    // CAS failed (другой поток обновил expected) → retry loop
}
```

**Почему Eve не может обойти:**
1. `pkt_id < expected` — early return **до** дешифрации → нет side channels
2. Atomic CAS — TOCTOU невозможен (проверка и обновление атомарны)
3. Monotonic counter — нет rollback, только вперёд

## NoiseSession

После завершения [Noise handshake](data/projects/GoodNet/docs/protocol/noise-handshake.md) создаётся `NoiseSession`:

```
NoiseSession:
    send_key        [32]   — ключ шифрования исходящих пакетов
    recv_key        [32]   — ключ дешифрации входящих пакетов
    handshake_hash  [32]   — финальный hash Noise handshake (channel binding)
```

Ключи направленные: `initiator.send_key == responder.recv_key` и наоборот. Noise Split гарантирует это.

## Noise KDF chain

Noise использует chaining key (ck) для деривации ключей через HKDF на основе BLAKE2b keyed hash:

```
MixKey(ck, input_key_material):
    (ck, k) = HKDF(ck, ikm)

HKDF:
    temp_key = BLAKE2b-keyed(key=ck, data=ikm)
    output1  = BLAKE2b-keyed(key=temp_key, data=0x01)
    output2  = BLAKE2b-keyed(key=temp_key, data=output1 || 0x02)
```

Каждая DH-операция (`ee`, `es`, `se`) вызывает `MixKey`, подмешивая shared secret в chaining key.

## Rekey

Обновление session keys без отправки сообщений:

```
Rekey(key):
    key = HKDF(key, zeros[32])
```

Свойства:
- **Без сообщений**: не требует KEY_EXCHANGE messages
- **Forward secrecy**: старый ключ нельзя восстановить из нового
- **Синхронность**: обе стороны вызывают rekey в одинаковой точке

## Forward secrecy

Компрометация identity keys (user_sk, device_sk) **не** раскрывает прошлые session keys:
- Ephemeral keys генерируются для каждого handshake внутри Noise HandshakeState
- Ephemeral SK затирается после Split
- Session keys = результат KDF chain, включающего ephemeral DH (`ee`)

Компрометация session key одного соединения не влияет на другие — каждое соединение имеет свой ephemeral keypair.

## Модель угроз

| Угроза | Защита |
|--------|--------|
| Прослушка трафика | ChaChaPoly-IETF AEAD |
| Replay attack | Monotonic nonce (packet_id) + CAS |
| Man-in-the-middle | [Noise handshake](data/projects/GoodNet/docs/protocol/noise-handshake.md) + [cross-verification](data/projects/GoodNet/docs/protocol/noise-handshake.md#cross-verification) (Ed25519 → X25519) |
| Подмена sender | Noise session binding (identity привязана DH-операциями) |
| Identity hiding | XX pattern: static keys в зашифрованных Noise payload |
| Компрометация identity key | Forward secrecy (ephemeral DH + Noise KDF chain) |
| Компрометация device | User key отдельный, можно отозвать |
| Identity substitution | Cross-verification: Ed25519 → X25519 + сравнение с Noise rs |
| DoS garbage data | close_now при bad magic/proto_ver |
| TRUSTED spoofing | TRUSTED принимается только от is_localhost |
| Tampered payload | Poly1305 MAC (AEAD decrypt fails) |

### Не защищает от

- **Traffic analysis**: размеры пакетов и timing видны (нет padding)
  - Пассивный наблюдатель видит: payload_len в header (plaintext), частоту пакетов, burst patterns
  - Correlation attack: сопоставить размеры отправленных/полученных пакетов между узлами → вычислить топологию mesh
  - Mitigation (roadmap v1.0): constant-length padding, dummy traffic

- **Endpoint correlation**: IP адреса не маскируются (нет onion routing)
  - ISP/государство видит: кто с кем связывается (IP-пары в TCP/UDP пакетах)
  - Даже с [relay](data/projects/GoodNet/docs/architecture/connection-manager.md#gossip-relay), прямые TCP соединения раскрывают graph структуру
  - Mitigation (roadmap v2.0): Mix networks, onion routing через доверенные узлы

- **Key distribution**: нет PKI / trust-on-first-use (TOFU) policy
  - **КРИТИЧНАЯ УЯЗВИМОСТЬ**: При первом handshake нет верификации pubkey вне канала
  - MitM может перехватить первый handshake → установить два отдельных Noise session с Alice и Bob → relay всё traffic
  - Alice и Bob видят корректные Noise handshake (ee,es,se DH проходят), не зная что general_pubkey подменён
  - **Текущая рекомендация**: Сравнить user_pubkey вне канала (Signal-style safety numbers, QR код, голосовой звонок)
  - Mitigation (roadmap v1.0): Certificate transparency log, WoT (Web of Trust), blockchain-based identity registry

- **Quantum computers**: Ed25519 и X25519 уязвимы к Shor's algorithm
  - Post-quantum migration путь: гибридная схема (X25519 + Kyber768) в Noise handshake
  - Forward secrecy частично помогает: записанный сегодня трафик можно дешифровать при компрометации identity keys через квантовый компьютер, но ephemeral session keys (результат DH) защищены только до компрометации static keys
  - Mitigation (roadmap v2.0+): Noise_XXpsk3 + post-quantum KEM

- **Denial of Service (DoS)**:
  - Handshake flood: злоумышленник отправляет NOISE_INIT → Core allocates HandshakeState → memory exhaustion
  - Текущая защита: close_now при bad magic (zero-cost drop), но валидные NOISE_INIT проходят
  - Mitigation (roadmap): Proof-of-work для handshake, rate limiting per source IP

- **Compromised endpoints**:
  - Если device скомпрометирован (root access, keylogger), злоумышленник видит весь plaintext traffic
  - device_sk + user_sk в памяти → может выдавать себя за устройство/пользователя
  - Mitigation: hardware security modules (TPM 2.0 для device_sk), encrypted RAM, secure boot

- **Side channels**:
  - Timing attacks на ChaChaPoly implementation (libsodium constant-time → безопасно)
  - Cache timing (Spectre/Meltdown класс): libsodium не гарантирует защиту
  - Power analysis при физическом доступе к устройству

**Честное предупреждение:**
GoodNet в текущем виде (Alpha 0.1.0) подходит для защиты от пассивного прослушивания и оппортунистического MitM (ISP DPI, публичный WiFi). Для threat models с активным государственным противником требуется дополнительная защита (PKI, traffic padding, onion routing) — см. [roadmap](data/projects/GoodNet/docs/roadmap.md).

## Затирание секретов

Все secret keys затираются через `sodium_memzero()`:
- `NoiseSession` деструктор: `send_key`, `recv_key`, `handshake_hash`
- `HandshakeState` деструктор: ephemeral SK, chaining key, промежуточные ключи
- `NodeIdentity` деструктор: `user_seckey`, `device_seckey`
- DH shared secrets: затираются сразу после MixKey

---

**См. также:** [Noise_XX handshake](data/projects/GoodNet/docs/protocol/noise-handshake.md) · [Wire format](data/projects/GoodNet/docs/protocol/wire-format.md) · [Конфигурация](data/projects/GoodNet/docs/config.md)

## HandshakePayload signature

### Signing (отправитель)

**Из `cm_handshake.cpp::build_handshake_payload()`:**

```cpp
msg::HandshakePayload hp{};
std::memcpy(hp.user_pubkey,   identity_.user_pubkey,   32);
std::memcpy(hp.device_pubkey, identity_.device_pubkey, 32);

// Подпись: sig = Ed25519(user_sk, user_pk || device_pk)
uint8_t to_sign[64];  // 32 + 32
std::memcpy(to_sign,      hp.user_pubkey,   32);
std::memcpy(to_sign + 32, hp.device_pubkey, 32);

crypto_sign_ed25519_detached(
    hp.signature, nullptr,
    to_sign, 64,
    identity_.user_seckey);  // user secret key!
```

**Цель подписи:**
1. Доказывает владение `user_seckey` (без sk невозможно создать валидную подпись)
2. Связывает `user_pubkey` с `device_pubkey` (подпись покрывает оба)
3. Предотвращает подмену: Eve не может взять `Alice_user_pk` + `Eve_device_pk` и подписать

### Verification (получатель)

**Из `cm_handshake.cpp::process_handshake_payload()`:**

```cpp
const auto* hp = reinterpret_cast<const msg::HandshakePayload*>(data);

// 1. Проверка подписи
uint8_t to_verify[64];
std::memcpy(to_verify,      hp->user_pubkey,   32);
std::memcpy(to_verify + 32, hp->device_pubkey, 32);

if (crypto_sign_ed25519_verify_detached(
        hp->signature,
        to_verify, 64,
        hp->user_pubkey) != 0)  // Проверяем против user_pubkey из payload
{
    LOG_WARN("Invalid signature — dropping");
    bus_.emit_drop(id, DropReason::AuthFail);
    close_now(id);
    return false;
}

// 2. Cross-verification: device_pk должен совпадать с Noise rs
uint8_t expected_x25519[32];
crypto_sign_ed25519_pk_to_curve25519(expected_x25519, hp->device_pubkey);

auto rec = rcu_find(id);
if (!rec || !rec->handshake) return false;

const uint8_t* noise_rs = rec->handshake->get_remote_static();
if (std::memcmp(expected_x25519, noise_rs, 32) != 0) {
    LOG_WARN("Cross-verification failed: device_pk mismatch with Noise rs");
    close_now(id);
    return false;
}
```

**Двойная проверка:**
1. **Signature verification**: подтверждает что sender владеет `user_seckey`
2. **Cross-verification**: подтверждает что `device_pk` в payload соответствует Noise static key (rs)

**Защита:**
- Eve не может подменить `device_pk` без компрометации `user_sk`
- Даже если Eve скомпрометирует `user_sk`, она не может подменить Noise static key (привязан через DH)
- Атака требует компрометации И user_sk И device_sk → двойной барьер

### Security analysis: MitM на первом handshake

**Проблема (TOFU vulnerability):**
При первом contact с peer нет out-of-band verification → MitM может перехватить handshake.

**Текущая защита:**
1. **Signature + cross-verification** усложняют атаку:
   - MitM должен подменить оба ключа (user + device)
   - Но не может создать валидную подпись без user_sk
   
2. **Noise_XX DH binding**:
   - Static keys привязаны через DH operations (ee, es, se)
   - Подмена static key требует контроля над ephemeral keys обеих сторон

**Ограничения:**
- **Active MitM на первом контакте**: если Eve контролирует канал при первом handshake, она может установить два отдельных Noise session (Alice ↔ Eve ↔ Bob) и relay весь traffic
- **Trust-on-first-use**: после первого handshake peer_pubkey запоминается → последующие подмены обнаружатся

**Mitigation (roadmap v1.0):**
- **Out-of-band verification**: QR код, safety numbers (Signal-style)
- **PKI / Certificate transparency**: centralized или blockchain-based identity registry
- **WoT (Web of Trust)**: peer endorsements, reputation scores

