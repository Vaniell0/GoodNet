# 08 — Identity and Hardware Binding

`core/cm_identity.cpp` · `core/data/machine_id.hpp`

---

## NodeIdentity

```cpp
struct NodeIdentity {
    uint8_t user_pubkey  [32]; // Ed25519 pub — user identification
    uint8_t user_seckey  [64]; // Ed25519 sec — signs AUTH packets
    uint8_t device_pubkey[32]; // Ed25519 pub — device identification
    uint8_t device_seckey[64]; // Ed25519 sec — signs packet headers
};
```

Created via `NodeIdentity::load_or_generate(IdentityConfig)`.

---

## user_key Loading

```
Priority:

1. cfg.ssh_key_path
   └── try_load_ssh_key(path)
         PEM → strip header/footer
         base64 → sodium_base642bin(VARIANT_ORIGINAL)
         parse openssh-key-v1 binary block
         cipher == "none"?          → else WARN + return false
         key_type == "ssh-ed25519"? → else WARN + return false
         extract pub[32], sec[64]

2. ~/.ssh/id_ed25519  (HOME / USERPROFILE)
   └── same parser

3. Generate
   crypto_sign_keypair(pub, sec)
   save_key(<dir>/user_key, sec, 64)  →  chmod 0600 (Linux/macOS)
```

---

## device_key Derivation

```cpp
const std::string mid = MachineId::get_or_create(cfg.dir);
MachineId::derive_device_keypair(mid, id.user_pubkey,
                                  id.device_pubkey, id.device_seckey);

// Inside derive_device_keypair():
uint8_t seed[32];
crypto_generichash_state st;
crypto_generichash_init(&st, nullptr, 0, sizeof(seed));
crypto_generichash_update(&st, machine_id_bytes, mid.size());
crypto_generichash_update(&st, user_pubkey, 32);
crypto_generichash_final(&st, seed, sizeof(seed));
crypto_sign_seed_keypair(out_pub, out_sec, seed);
sodium_memzero(seed, sizeof(seed));
```

---

## MachineId — Sources by Platform

### Linux

| Source | Path | Root? |
|---|---|---|
| systemd machine-id | `/etc/machine-id` | No |
| dbus machine-id | `/var/lib/dbus/machine-id` | No |
| SMBIOS UUID | `/sys/class/dmi/id/product_uuid` | No |
| Board serial | `/sys/class/dmi/id/board_serial` | No |
| CPU serial (CPUID leaf 3) | x86/x64 only | No |
| Physical NIC MACs | `/sys/class/net/<iface>/address` + `/device` exists | No |

Only physical interfaces (check `/sys/class/net/<iface>/device` exists).
MAC `000000000000` — skipped. Locally-administered (`mac[0] & 0x02`) — skipped.

### macOS

| Source | Mechanism |
|---|---|
| Platform UUID | IOKit: `IOPlatformExpertDevice → IOPlatformUUID` |
| Serial number | IOKit: `IOPlatformExpertDevice → IOPlatformSerialNumber` |
| MAC addresses | `getifaddrs()`, `AF_LINK`, not locally-administered |

### Windows

| Source | Mechanism |
|---|---|
| MachineGuid | `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid` |
| SMBIOS UUID | `GetSystemFirmwareTable('RSMB', 0)` → type 1, offset 8 |
| MAC addresses | `GetAdaptersInfo()`, length 6, not locally-administered |

### Hashing Algorithm

```cpp
static std::string hash_sources(const std::vector<std::string>& sources) {
    uint8_t out[32];
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, sizeof(out));
    for (const auto& s : sources)
        crypto_generichash_update(&st,
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
    crypto_generichash_final(&st, out, sizeof(out));
    return bytes_to_hex(out, sizeof(out));  // 64-char hex string
}
```

### Fallback

If no source is available:
1. `randombytes_buf(rand_id, 32)`
2. Save to `<dir>/machine_id`
3. Read on subsequent starts

---

*← [07 — PluginManager](07-plugin-manager.md) · [09 — Logger →](09-logger.md)*
