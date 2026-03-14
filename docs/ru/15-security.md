# 15 — Безопасность

---

## Модель угроз

| Угроза | Защита |
|---|---|
| MITM при рукопожатии | Ed25519 подпись AUTH включает `ephem_pubkey` |
| Replay Attack на AUTH | `ephem_pk` одноразовый, включён в подпись |
| Replay Attack на пакеты | Монотонный `recv_nonce_expected` — nonce < expected → drop |
| Tamper (подделка пакетов) | Poly1305 MAC — любое изменение → MAC fail → drop |
| Подмена плагина | SHA-256 по манифесту до `dlopen` |
| Компрометация одного аккаунта | `device_key = f(machine_id, user_pk)` — уникален для пары |
| Перенос `device_key` на другую машину | Привязан к hardware ID — не воспроизводим |
| Утечка ephemeral ключей | `sodium_memzero` сразу после `derive_session()` |
| Утечка session_key | `sodium_memzero` в `~SessionState()` |
| Pre-auth flood | Пакеты до STATE_ESTABLISHED отбрасываются (кроме AUTH) |
| OOM из очереди отправки | Backpressure: `pending_bytes > 512 МБ` → drop + `stats_.backpressure` |
| Символьные конфликты между плагинами | `RTLD_LOCAL` изолирует каждый `.so` |
| Символьный конфликт имён хендлеров | `PROPAGATION_REJECT` + `stats_.rejected` |

---

## Примитивы шифрования

GoodNet использует `crypto_secretbox_easy` из libsodium — это **XSalsa20-Poly1305** (не ChaCha20). XSalsa20 использует 192-битный nonce вместо 96-битного у ChaCha20, что устраняет риск коллизии при случайной генерации nonce. GoodNet использует монотонный счётчик (`uint64_t`), поэтому nonce никогда не повторяется в рамках сессии.

```
XSalsa20-Poly1305 (crypto_secretbox_easy):
  nonce: 24 байта (8 = uint64_t LE + 16 нулей)
  MAC:   16 байт (Poly1305)
  ключ:  32 байта (session_key из ECDH+BLAKE2b-256)
```

---

## Localhost-оптимизация: безопасна ли?

Для `127.x.x.x` / `::1` крипто и Zstd пропускаются. Обоснование:
- AUTH всё равно выполняется — пир идентифицирован
- Localhost-трафик не покидает machine — шифрование бессмысленно
- Производительность IPC критична для некоторых применений

Не используйте localhost-режим для трафика, который может быть перехвачен (container-to-container без изоляции сети).

---

## Ограничения (alpha)

### Нет PFS в строгом смысле

Если `user_seckey` скомпрометирован задним числом + перехвачен весь трафик → можно восстановить `session_key` из AUTH. Настоящий PFS требует ephemeral signing.

### Нет double ratchet

Сессионный ключ фиксирован на всё время TCP-соединения.

### Нет rate limiting в ядре

Защита от DoS реализуется на уровне хендлера:

```cpp
propagation_t on_result(const header_t*, uint32_t) override {
    if (++auth_attempts_[remote_ip_] > MAX_AUTH_ATTEMPTS)
        return PROPAGATION_REJECT;  // дроп + счётчик rejected++
    return PROPAGATION_CONTINUE;
}
```

### Нет certificate pinning

Нет глобального PKI. Доверие строится на Out-of-Band обмене `user_pubkey` (QR-код, мессенджер, физическое присутствие).

### CoreMeta обратная совместимость

Старые клиенты присылают `payload_len == kBaseSize` (160 байт) без `CoreMeta`. Ядро трактует `peer_core_meta = {0, 0}` как "версия неизвестна, возможности неизвестны". Ни один флаг `CORE_CAP_*` не включается автоматически.

---

*← [14 — Тестирование](14-testing.md) · [16 — Roadmap →](16-roadmap.md)*
