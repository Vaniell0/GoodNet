# 09 — Безопасность

---

## Модель угроз

| Угроза | Защита |
|---|---|
| MITM при рукопожатии | Ed25519 подпись AUTH включает `ephem_pubkey` |
| Replay AUTH | `ephem_pk` одноразовый, включён в подпись |
| Replay пакетов | Монотонный `recv_nonce_expected` → nonce < expected = drop |
| Tamper (подделка) | Poly1305 MAC — изменение → MAC fail → drop |
| Подмена плагина | SHA-256 верификация до `dlopen` |
| Компрометация аккаунта | `device_key = f(machine_id, user_pk)` — уникален для пары устройство+пользователь |
| Перенос device_key | Привязан к hardware ID — не воспроизводим на другой машине |
| Утечка ephemeral ключей | `sodium_memzero` сразу после `derive_session()` |
| Утечка session_key | `sodium_memzero` в `~SessionState()` |
| Pre-auth flood | Пакеты до STATE_ESTABLISHED отбрасываются (кроме AUTH) |
| OOM очереди отправки | Global backpressure: 512 MB лимит → drop + stats |
| Per-connection flood | PerConnQueue: 8 MB лимит → drop |
| Символьные конфликты плагинов | `RTLD_LOCAL` изолирует каждый `.so` |
| GNET_FLAG_TRUSTED injection | `TrustedFromRemote` drop — флаг можно выставить только локально |

---

## Криптографические примитивы

| Примитив | Алгоритм | Применение |
|---|---|---|
| Подпись / идентификация | Ed25519 | user_key, device_key |
| ECDH | X25519 | session key derivation |
| KDF | BLAKE2b-256 | `session_key = BLAKE2b(shared \|\| min(pk1,pk2) \|\| max(pk1,pk2))` |
| AEAD шифрование | XSalsa20-Poly1305 | весь трафик после ESTABLISHED |
| Nonce | 24 байта (8 = uint64_t LE + 16 нулей) | монотонный, replay protection |
| Хэширование плагинов | SHA-256 | верификация `.so` до dlopen |
| Hardware fingerprint | BLAKE2b | MachineId → device_key derivation |

**Важно**: используется `crypto_secretbox_easy` (XSalsa20-Poly1305), **не** ChaCha20-Poly1305. XSalsa20 использует 192-bit nonce, но GoodNet применяет монотонный счётчик — коллизия nonce исключена.

---

## Зоны доверия

```
┌─────────────────────────────────────────┐
│          Localhost (127.x / ::1)        │
│  AUTH выполняется, крипто пропускается  │
│  GNET_FLAG_TRUSTED разрешён             │
└────────────────────┬────────────────────┘
                     │
┌────────────────────▼────────────────────┐
│            Remote connections           │
│  Полный цикл: AUTH + ECDH + encrypt    │
│  GNET_FLAG_TRUSTED → drop              │
└─────────────────────────────────────────┘
```

Localhost-оптимизация безопасна потому что:
- AUTH всё равно выполняется — пир идентифицирован
- Трафик не покидает машину
- IPC-производительность критична

**Не используйте** localhost-режим для container-to-container трафика без сетевой изоляции.

---

## Секретная память

Все криптографические ключи очищаются через `sodium_memzero()`:

| Ключ | Когда очищается |
|---|---|
| `ephem_seckey` (X25519) | Сразу после `derive_session()` |
| `shared_secret` (ECDH) | Сразу после BLAKE2b KDF |
| `session_key` | В `~SessionState()` деструкторе |
| `user_seckey` (Ed25519) | При `rotate_identity_keys()` или `~Core()` |

---

## Ограничения (alpha)

### Нет PFS в строгом смысле

Если `user_seckey` скомпрометирован задним числом и весь трафик перехвачен → можно восстановить `session_key` из AUTH (подпись user_key позволяет знать ephem_key). Настоящий PFS требует ephemeral signing keys.

### Нет double ratchet

Session key фиксирован на время TCP-соединения. `rekey_session()` позволяет ручную ротацию.

### Нет rate limiting в ядре

Защита от DoS реализуется через хендлер-плагин:

```cpp
propagation_t on_result(const header_t*, uint32_t) override {
    if (rate_exceeded(remote_ip_))
        return PROPAGATION_REJECT;
    return PROPAGATION_CONTINUE;
}
```

### Нет certificate pinning / PKI

Доверие строится на Out-of-Band обмене `user_pubkey` (QR-код, мессенджер).

### CoreMeta обратная совместимость

Старые клиенты: `payload_len == kBaseSize` → `peer_core_meta = {0, 0}` → все CORE_CAP_* выключены.

---

*← [08 — Протокол](08-protocol.md) · [10 — Конфигурация →](10-config.md)*
