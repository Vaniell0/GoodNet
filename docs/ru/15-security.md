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
| Символьные конфликты между плагинами | `RTLD_LOCAL` изолирует каждый `.so` |

---

## Localhost-оптимизация: безопасна ли?

Для `127.x.x.x` / `::1` крипто пропускается. Обоснование:
- AUTH всё равно выполняется — пир идентифицирован
- Localhost-трафик не покидает machine — шифрование бессмысленно
- Производительность IPC критична для некоторых применений

Не используйте localhost-режим для трафика, который теоретически может быть перехвачен (container-to-container без изоляции сети).

---

## Ограничения (alpha)

### Нет PFS в строгом смысле

Если `user_seckey` скомпрометирован задним числом + перехвачен весь трафик → можно восстановить `session_key` из AUTH. Настоящий PFS требует ephemeral signing.

### Нет double ratchet

Сессионный ключ фиксирован на всё время TCP-соединения.

### Нет rate limiting в ядре

Защита от DoS реализуется на уровне хендлера. Рекомендуемая схема:

```cpp
// В обработчике AUTH/connect:
if (++auth_attempts[remote_ip] > Config::Security::MAX_AUTH_ATTEMPTS)
    disconnect(id);  // api->on_disconnect(...)

if (auth_timer.elapsed() > Config::Security::KEY_EXCHANGE_TIMEOUT)
    disconnect(id);
```

### Нет certificate pinning

Нет глобального PKI. Доверие строится на Out-of-Band обмене `user_pubkey` (например, через QR-код, мессенджер, физическое присутствие).

---

*← [14 — Тестирование](14-testing.md) · [16 — Roadmap →](16-roadmap.md)*
