# 08 — Идентификация и аппаратная привязка

`core/cm_identity.cpp` · `core/data/machine_id.hpp`

---

## NodeIdentity

```cpp
struct NodeIdentity {
    uint8_t user_pubkey  [32]; // Ed25519 pub — идентификация пользователя
    uint8_t user_seckey  [64]; // Ed25519 sec — подпись AUTH пакетов
    uint8_t device_pubkey[32]; // Ed25519 pub — идентификация устройства
    uint8_t device_seckey[64]; // Ed25519 sec — подпись заголовков
};
```

Создаётся через `NodeIdentity::load_or_generate(IdentityConfig)`.

---

## IdentityConfig

```cpp
struct IdentityConfig {
    fs::path dir            = "~/.goodnet"; // ключи и machine_id файл
    fs::path ssh_key_path;                  // пусто = автодетект
    bool     use_machine_id = true;         // привязать device_key к железу
};
```

---

## Загрузка user_key

```
Приоритет:

1. cfg.ssh_key_path
   └── try_load_ssh_key(path)
         PEM → strip header/footer
         base64 → sodium_base642bin(VARIANT_ORIGINAL)
         парсинг openssh-key-v1 блока
         cipher == "none"?        → иначе WARN + return false
         key_type == "ssh-ed25519"? → иначе WARN + return false
         извлекаем pub[32], sec[64]

2. ~/.ssh/id_ed25519  (HOME / USERPROFILE)
   └── тот же парсер

3. Генерация
   crypto_sign_keypair(pub, sec)
   save_key(<dir>/user_key, sec, 64)  →  chmod 0600 (Linux/macOS)
   LOG_INFO("Generated keypair")
```

Формат файла `user_key`: 64 байта raw libsodium Ed25519 seckey = seed[32] ‖ pubkey[32].
При загрузке: `pub = sec[32..63]` (последние 32 байта seckey == pubkey в libsodium).

---

## Деривация device_key

```cpp
// cm_identity.cpp: load_or_generate()
if (cfg.use_machine_id) {
    const std::string mid = MachineId::get_or_create(cfg.dir);
    if (!mid.empty()) {
        MachineId::derive_device_keypair(
            mid, id.user_pubkey,
            id.device_pubkey, id.device_seckey);
        LOG_INFO("Device key: hardware-bound");
    }
}
```

```cpp
// machine_id.hpp: derive_device_keypair()
void derive_device_keypair(const std::string& machine_id,
                            const uint8_t user_pubkey[32],
                            uint8_t out_pub[32], uint8_t out_sec[64])
{
    uint8_t seed[32];
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, sizeof(seed));
    crypto_generichash_update(&st,
        reinterpret_cast<const uint8_t*>(machine_id.data()),
        machine_id.size());
    crypto_generichash_update(&st, user_pubkey, 32);
    crypto_generichash_final(&st, seed, sizeof(seed));

    crypto_sign_seed_keypair(out_pub, out_sec, seed);
    sodium_memzero(seed, sizeof(seed));
}
```

---

## MachineId — источники по платформам

### Linux

| Источник | Путь | Root? |
|---|---|---|
| systemd machine-id | `/etc/machine-id` | Нет |
| dbus machine-id | `/var/lib/dbus/machine-id` | Нет |
| SMBIOS UUID | `/sys/class/dmi/id/product_uuid` | Нет |
| Серийный номер платы | `/sys/class/dmi/id/board_serial` | Нет |
| CPU serial (CPUID leaf 3) | `__cpuid(3, …)` x86/x64 | Нет |
| MAC физических сетевых карт | `/sys/class/net/<iface>/address` + `/device` | Нет |

Только физические интерфейсы: проверяется наличие `/sys/class/net/<iface>/device`.
MAC `000000000000` — пропускается. Locally-administered (бит `mac[0] & 0x02`) — пропускается.

### macOS

| Источник | Механизм |
|---|---|
| Platform UUID | IOKit: `IOPlatformExpertDevice → IOPlatformUUID` |
| Serial number | IOKit: `IOPlatformExpertDevice → IOPlatformSerialNumber` |
| MAC адреса | `getifaddrs()`, `AF_LINK`, не locally-administered |

### Windows

| Источник | Механизм |
|---|---|
| MachineGuid | `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid` |
| SMBIOS UUID | `GetSystemFirmwareTable('RSMB', 0)` → type 1, offset 8 |
| MAC адреса | `GetAdaptersInfo()`, длина 6, не locally-administered |

---

## Алгоритм хэширования источников

```cpp
static std::string hash_sources(const std::vector<std::string>& sources) {
    uint8_t out[32];
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, sizeof(out));
    for (const auto& s : sources)
        crypto_generichash_update(&st,
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    crypto_generichash_final(&st, out, sizeof(out));
    return bytes_to_hex(out, sizeof(out)); // 64-символьный hex
}
```

Чем больше источников собрано — тем стабильнее и уникальнее ID.

---

## Fallback

Если ни один источник недоступен:
1. `randombytes_buf(rand_id, 32)` — генерируем случайный ID
2. Сохраняем в `<dir>/machine_id` (hex строка)
3. При следующих запусках читаем его же

---

*← [07 — PluginManager](07-plugin-manager.md) · [09 — Logger →](09-logger.md)*
