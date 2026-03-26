# Noise_XX Handshake

GoodNet использует Noise_XX pattern (`Noise_XX_25519_ChaChaPoly_BLAKE2b`). Строго упорядоченный 3-message обмен с ролями Initiator и Responder. Реализация: `core/crypto/noise.hpp`, `core/crypto/noise.cpp`, `core/cm/handshake.cpp`.

См. также: [Криптография](../protocol/crypto.md) · [Wire format](../protocol/wire-format.md) · [ConnectionManager](../architecture/connection-manager.md)

## Noise_XX pattern

```
Noise_XX:
  → e                          (msg1: initiator sends ephemeral)
  ← e, ee, s, es               (msg2: responder sends ephemeral + static)
  → s, se                       (msg3: initiator sends static)
```

- `e` — ephemeral public key (X25519)
- `s` — static public key (X25519, derived from device key via `ed25519_pk_to_curve25519`)
- `ee, es, se` — DH operations между ephemeral/static ключами сторон

XX означает: обе стороны передают свои static keys в зашифрованном виде. Ни initiator, ни responder не знают друг друга заранее — подходит для zero-trust mesh-сети.

## HandshakePayload

265 bytes, передаётся внутри Noise msg2 (responder) и msg3 (initiator). В msg1 HandshakePayload **не отправляется** — только ephemeral key:

```
struct HandshakePayload {
    user_pubkey    [32]     // Ed25519 user public key
    device_pubkey  [32]     // Ed25519 device public key
    signature      [64]     // Ed25519(user_sk, user_pk || device_pk)
    schemes_count  [1]      // количество транспортов
    schemes        [8][16]  // поддерживаемые транспорты
    core_meta      [8]      // версия ядра + capabilities
};
```

**signature** — подпись user secret key над конкатенацией `user_pk || device_pk`. Доказывает владение user_sk и что device_pk не подменён.

**schemes** — список транспортов ("tcp", "ice"). Используется для negotiation.

## 3-message exchange

```
Initiator                           Responder
  │                                   │
  ├─── NOISE_INIT (type=1) ────────► │
  │    Noise msg1: →e                 │
  │    (только ephemeral key, 32 B)   │
  │                                   │
  │ ◄──────── NOISE_RESP (type=2) ── ┤
  │    Noise msg2: ←e,ee,s,es        │
  │    + HandshakePayload (265 B)     │
  │    (зашифровано Noise-ключами)    │
  │                                   │
  ├─── NOISE_FIN (type=3) ─────────► │
  │    Noise msg3: →s,se             │
  │    + HandshakePayload (265 B)     │
  │    (зашифровано Noise-ключами)    │
  │                                   │
  │ ── ESTABLISHED ──────────────── ─┤
  │    (ChaChaPoly на session keys)   │
```

Инициатор определяется флагом `EP_FLAG_OUTBOUND` (0x02) при создании соединения. Только инициатор отправляет NOISE_INIT. Респондер ждёт.

### Detailed message breakdown (field-level)

#### msg1 (NOISE_INIT): → e (без payload)

msg1 содержит **только** ephemeral key. HandshakePayload здесь не передаётся — он отправляется в msg2 и msg3. Код: `send_noise_init()` вызывает `write_message(nullptr, 0, ...)`.

```
┌─────────────────────────────────────────────────────────────┐
│ header_t (20 bytes)                                         │
├─────────────────────────────────────────────────────────────┤
│ magic: 0x474E4554 ('GNET')                                  │
│ proto_ver: 3                                                │
│ flags: 0x00                                                 │
│ payload_type: 1 (MSG_TYPE_NOISE_INIT)                       │
│ payload_len: 32                                             │
│ packet_id: 0                                                │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ Payload (32 bytes, plaintext)                               │
├─────────────────────────────────────────────────────────────┤
│ ephemeral_pubkey [32]    ← initiator ephemeral X25519 pk   │
└─────────────────────────────────────────────────────────────┘
```

**Состояние Noise после msg1:**
- Initiator: `HandshakeState` с локальным ephemeral keypair
- Responder: получил `initiator.e`, вычислил промежуточный handshake_hash

#### msg2 (NOISE_RESP): ← e, ee, s, es + encrypted HandshakePayload

Responder читает msg1, затем вызывает `write_message(hp_bytes)` с HandshakePayload.

```
┌─────────────────────────────────────────────────────────────┐
│ header_t (20 bytes)                                         │
├─────────────────────────────────────────────────────────────┤
│ payload_type: 2 (MSG_TYPE_NOISE_RESP)                       │
│ payload_len: ~377 bytes                                     │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ Payload (~377 bytes)                                        │
├─────────────────────────────────────────────────────────────┤
│ ephemeral_pubkey [32]        ← responder ephemeral X25519   │
│ + MAC            [16]        ← Poly1305 (после e token)     │
├─────────────────────────────────────────────────────────────┤
│ (ee DH, es DH → MixKey)                                    │
├─────────────────────────────────────────────────────────────┤
│ Encrypted static_pubkey [32] ← responder static X25519 pk   │
│ + MAC            [16]        ← Poly1305 (после s token)     │
├─────────────────────────────────────────────────────────────┤
│ Encrypted HandshakePayload [265] ← responder identity       │
│ + MAC            [16]        ← Poly1305 (payload)           │
└─────────────────────────────────────────────────────────────┘
```

**Crypto operations:**
1. `ee` = DH(initiator.e, responder.e) — ephemeral-ephemeral
2. `es` = DH(initiator.e, responder.s) — ephemeral-static
3. MixKey(ee) → update chaining key
4. MixKey(es) → update chaining key
5. Encrypt(static_pk + HandshakePayload) с промежуточным cipher key

**Защита:** Responder static key зашифрован → passive observer не видит identity.

#### msg3 (NOISE_FIN): → s, se + encrypted HandshakePayload

Initiator читает msg2, обрабатывает HandshakePayload респондера, затем отправляет свой HandshakePayload.

```
┌─────────────────────────────────────────────────────────────┐
│ header_t (20 bytes)                                         │
├─────────────────────────────────────────────────────────────┤
│ payload_type: 3 (MSG_TYPE_NOISE_FIN)                        │
│ payload_len: ~329 bytes                                     │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ Payload (~329 bytes)                                        │
├─────────────────────────────────────────────────────────────┤
│ Encrypted static_pubkey [32] ← initiator static X25519 pk   │
│ + MAC            [16]        ← Poly1305 (после s token)     │
├─────────────────────────────────────────────────────────────┤
│ (se DH → MixKey)                                            │
├─────────────────────────────────────────────────────────────┤
│ Encrypted HandshakePayload [265] ← initiator identity       │
│ + MAC            [16]        ← Poly1305 (payload)           │
└─────────────────────────────────────────────────────────────┘
```

**Crypto operations:**
1. `se` = DH(initiator.s, responder.e) — static-ephemeral
2. MixKey(se) → final chaining key
3. Split() → (send_key, recv_key) из chaining key
4. Encrypt(static_pk + HandshakePayload) с финальным cipher key

**Защита:** Initiator static key зашифрован после **трёх** DH operations → максимальная защита от активного MitM.

**После msg3:** Обе стороны имеют `(send_key, recv_key)` → STATE_ESTABLISHED.

## Cross-verification

При обработке HandshakePayload обе стороны выполняют cross-verification:

```
process_handshake_payload(peer_payload, noise_remote_static_key):
  1. Конвертировать peer Ed25519 device_pubkey → X25519:
       expected_x25519 = crypto_sign_ed25519_pk_to_curve25519(peer_device_pubkey)
  2. Сравнить с Noise remote static key (rs):
       memcmp(expected_x25519, noise_rs, 32) == 0 ?
  3. Если не совпадает → close_now() (identity substitution attempt)
  4. Verify signature:
       verify_detached(sig, user_pk || device_pk, peer_user_pubkey) → OK?
```

**Важно:** Noise static key — это X25519 ключ (для DH-операций), а HandshakePayload содержит Ed25519 `device_pubkey` (для подписи). Это **разные** типы ключей. Cross-verification конвертирует Ed25519 `device_pubkey` в X25519 через `crypto_sign_ed25519_pk_to_curve25519()` и сравнивает результат с Noise `rs` (remote static). Если не совпадает — peer подменил `device_pubkey` в payload.

Шаг 2 предотвращает атаку identity substitution: злоумышленник не может подменить device_pubkey в HandshakePayload — Noise static key привязан DH-операциями в [chaining key](../protocol/crypto.md#noise-kdf-chain).

### Identity hiding

- **Responder**: раскрывает static key в msg2 (зашифрован после `ee` DH — защита от пассивного наблюдателя)
- **Initiator**: раскрывает static key в msg3 (зашифрован после `ee` + `es` — защита и от пассивного наблюдателя, и от активного MitM)

### Identity substitution attack walkthrough

**Сценарий атаки:** Evil Eve пытается выдать себя за Alice, подменив device_pubkey в HandshakePayload.

```
Время  Операция                         Eve's план             Защита
─────  ──────────────────────────────── ─────────────────────  ─────────────────────
  t0   Eve перехватывает msg1 от Bob
  t1   Eve генерирует свой ephemeral_e'
  t2   Eve создаёт HandshakePayload:
        - user_pubkey = Alice_user_pk    ✅ Скопировано        (выглядит как Alice)
        - device_pubkey = Eve_device_pk  ❌ ПОДМЕНА!           (Eve's device key)
        - signature = ?
  t3   Eve пытается подписать:
        sig = Ed25519_sign(Alice_user_sk, Alice_user_pk || Eve_device_pk)
                            ^^^^^^^^^^^^
                            У Eve НЕТ Alice secret key!
       → Подпись FAILED (Eve не может подписать без Alice_user_sk)

  t4   Eve отправляет msg2 с некорректной подписью

  t5   Bob получает msg2, начинает cross-verification:
        1. Decrypt HandshakePayload (Noise уже расшифровал)
        2. Извлечь device_pubkey = Eve_device_pk
        3. Ed25519_pk_to_curve25519(Eve_device_pk) → Eve_x25519_pk
        4. Сравнить с Noise rs (remote static key):
           memcmp(Eve_x25519_pk, Alice_x25519_pk) != 0  ❌ MISMATCH!
        5. close_now(conn_id)  ← connection terminated

Результат: Атака провалилась на шаге 5 (cross-verification).
```

**Почему Eve не может подделать signature?**

Signature требует Alice user_sk:
```
signature = Ed25519_sign(Alice_user_sk, user_pk || device_pk)
```

Без Alice_user_sk Eve может только:
1. ❌ Подписать своим Eve_user_sk → signature проверится против Eve_user_pk (не Alice)
2. ❌ Replay старую подпись Alice → будет проверять `Alice_user_pk || Alice_device_pk`, не `Alice_user_pk || Eve_device_pk`

**Почему Eve не может подделать Noise static key?**

Noise static key (X25519) привязан к DH operations:
- msg2: Eve отправляет свой static_pk' → Bob вычисляет DH(Bob.e, Eve.s')
- Chaining key MixKey(DH результат) → зависит от Eve.s'
- Cross-verification: Ed25519→X25519(device_pk) **должен совпадать** с Noise static_pk'

Если Eve подменяет device_pk в HandshakePayload, но использует свой Noise static key → **mismatch** → close_now().

**Двойная защита:**
1. Signature verification (Ed25519) — доказывает владение user_sk
2. Cross-verification (Ed25519→X25519 vs Noise rs) — доказывает соответствие device_pk и Noise static key

Обе проверки должны пройти. Атакующий **не может** подменить identity без компрометации user_sk.

## EP_FLAG_OUTBOUND

Флаг `EP_FLAG_OUTBOUND` (0x02) в `endpoint_t::flags` определяет роль в handshake:
- **Set** (мы инициировали соединение) → Initiator: отправляет NOISE_INIT
- **Not set** (входящее соединение) → Responder: ждёт NOISE_INIT

## Rekey

Noise native rekey — без сообщений, без round-trip:

```
Обе стороны (по таймеру или триггеру):
  new_key = HKDF(current_key, zeros[32])
  packet_id продолжает расти (nonce уникальность сохраняется)
```

Rekey детерминирован — обе стороны вычисляют одинаковый новый ключ. Подробнее: [Криптография → Rekey](../protocol/crypto.md#rekey).

## Connection state machine

```
[new TCP] → STATE_CONNECTING
         → STATE_NOISE_HANDSHAKE
           → STATE_ESTABLISHED    (зашифрованный трафик)
             → STATE_CLOSING      (graceful drain)
               → STATE_CLOSED
```

---

**См. также:** [Криптография](../protocol/crypto.md) · [Wire format](../protocol/wire-format.md) · [ConnectionManager](../architecture/connection-manager.md)
