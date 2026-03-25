# Identity Migration

Руководства по переносу identity keys между машинами и сценарии использования.

См. также: [Криптография](../protocol/crypto.md) · [Конфигурация](../config.md)

## Перенос user identity на новую машину

**User key переносится**, device key **не переносится** (привязан к железу).

```bash
# Шаг 1: Скопировать user_key с машины A
scp machineA:~/.goodnet/user_key /tmp/user_key_backup

# Шаг 2: На машине B установить user_key
mkdir -p ~/.goodnet
cp /tmp/user_key_backup ~/.goodnet/user_key
chmod 600 ~/.goodnet/user_key

# Шаг 3: Удалить device_key (если существует)
rm -f ~/.goodnet/device_key  # Device key regenerate автоматически

# Шаг 4: Запустить Core
./goodnet
# [INFO] NodeIdentity: Loaded user key from ~/.goodnet/user_key
# [INFO] NodeIdentity: Generating device key (hardware-bound)...
# [INFO] Device pubkey: a1b2c3d4... (новый device key)
```

**Результат:**
- User pubkey одинаков на обеих машинах (A и B)
- Device pubkey разный (привязан к hardware fingerprint)
- Peers видят тот же user identity, но разный device identity

## Импорт из OpenSSH Ed25519

```bash
# Если уже есть ~/.ssh/id_ed25519 (OpenSSH keypair)
# можно импортировать как user_key

# config.json:
{
  "identity": {
    "ssh_key_path": "~/.ssh/id_ed25519"
  }
}

# Core при старте:
# [INFO] Importing user key from ~/.ssh/id_ed25519
# [INFO] User pubkey: <hex>
```

**Формат:** OpenSSH Ed25519 private key (RFC 8709). GoodNet читает только Ed25519 (не RSA, не ECDSA).

## Backup/restore всей identity

```bash
# Backup (сохранить user_key + device_key)
tar czf goodnet-identity-backup.tar.gz ~/.goodnet/

# Restore на ту же машину (например, после переустановки OS)
tar xzf goodnet-identity-backup.tar.gz -C ~/

# ВАЖНО: device_key валиден только если hardware fingerprint совпадает!
# Если DMI serial изменился (новая материнка) → device_key invalid
```

**Последствия invalid device_key:**
- Peers, запомнившие старый device_pk, отклонят handshake (cross-verification fail)
- Решение: удалить `device_key`, regenerate → новый device_pk → peers примут как новое устройство

## Multi-device setup (один user, несколько устройств)

```
User Alice имеет:
- Laptop (user_pk = 0xAAA..., device_pk = 0x111...)
- Desktop (user_pk = 0xAAA..., device_pk = 0x222...)
- Server (user_pk = 0xAAA..., device_pk = 0x333...)

Все устройства имеют одинаковый user_pk (скопирован user_key).
Все устройства имеют разный device_pk (hardware-bound).

Peers видят: user Alice (0xAAA) использует 3 устройства.
```

**Ротация device key:**
- Если laptop скомпрометирован → удалить его device_key из доверенных (roadmap: revocation list)
- User key остаётся валидным → можно regenerate device_key на laptop

---

**См. также:** [Криптография: ключи](../protocol/crypto.md#ключи) · [Конфигурация](../config.md)
