#pragma once

// ─── core/data/machine_id.hpp ─────────────────────────────────────────────────
//
// Привязка device_key к железу.
//
// Источники machine-id (в порядке приоритета):
//   1. /etc/machine-id          — systemd, уникален для установки
//   2. /var/lib/dbus/machine-id — dbus fallback
//   3. /sys/class/dmi/id/product_uuid (Linux, root не нужен)
//   4. Генерируем случайный + сохраняем в <dir>/machine_id
//
// Деривация ключа:
//   seed = BLAKE2b(machine_id || user_pubkey)  [domain separation]
//   device_keypair = Ed25519 seed → expand via libsodium crypto_sign_seed_keypair
//
// Зачем user_pubkey в seed:
//   Разные аккаунты на одной машине → разные device_key.
//   Компрометация одного аккаунта не раскрывает device_key другого.
//
// Безопасность:
//   • device_seckey детерминирован — восстанавливается из machine_id + user_pubkey.
//   • machine_id не является секретом (читается без root) — это OK:
//     злоумышленнику нужен и machine_id и user_seckey для деривации.
//   • device_key НЕ переносится между машинами — это цель.

#include <sodium.h>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace gn {

namespace fs = std::filesystem;

class MachineId {
public:
    // Получить строку machine-id (hex или uuid без дефисов).
    // Возвращает пустую строку если ничего не найдено.
    static std::string read() {
        // 1. /etc/machine-id
        if (auto id = read_file("/etc/machine-id"); !id.empty()) return id;
        // 2. /var/lib/dbus/machine-id
        if (auto id = read_file("/var/lib/dbus/machine-id"); !id.empty()) return id;
        // 3. /sys/class/dmi/id/product_uuid
        if (auto id = read_file("/sys/class/dmi/id/product_uuid"); !id.empty()) return id;
        return {};
    }

    // Получить или создать персистентный machine-id в <dir>/machine_id
    static std::string get_or_create(const fs::path& dir) {
        const fs::path path = dir / "machine_id";

        // Сначала пробуем системный
        auto system_id = read();
        if (!system_id.empty()) return system_id;

        // Затем сохранённый
        if (fs::exists(path)) {
            if (auto id = read_file(path); !id.empty()) return id;
        }

        // Генерируем случайный и сохраняем
        uint8_t rand_bytes[32];
        randombytes_buf(rand_bytes, sizeof(rand_bytes));

        std::string hex;
        hex.reserve(64);
        for (auto b : rand_bytes) {
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02x", b);
            hex += buf;
        }

        fs::create_directories(dir);
        std::ofstream f(path);
        if (f) { f << hex; f.close(); }

        return hex;
    }

    // Дерировать device keypair из machine_id + user_pubkey.
    // Детерминировано: одна машина + один аккаунт → один device_key.
    static void derive_device_keypair(
        std::string_view            machine_id,
        const uint8_t               user_pubkey[crypto_sign_PUBLICKEYBYTES],
        uint8_t                     out_pub[crypto_sign_PUBLICKEYBYTES],
        uint8_t                     out_sec[crypto_sign_SECRETKEYBYTES])
    {
        // Seed = BLAKE2b-256(machine_id_bytes || user_pubkey)
        // crypto_generichash = BLAKE2b
        uint8_t seed[crypto_sign_SEEDBYTES];  // 32 bytes

        crypto_generichash_state state;
        crypto_generichash_init(&state, nullptr, 0, sizeof(seed));
        crypto_generichash_update(&state,
            reinterpret_cast<const uint8_t*>(machine_id.data()),
            machine_id.size());
        crypto_generichash_update(&state, user_pubkey, crypto_sign_PUBLICKEYBYTES);
        crypto_generichash_final(&state, seed, sizeof(seed));

        // Ed25519 keypair из seed
        crypto_sign_seed_keypair(out_pub, out_sec, seed);

        // Затираем seed из памяти
        sodium_memzero(seed, sizeof(seed));
    }

private:
    static std::string read_file(const fs::path& path) {
        std::ifstream f(path);
        if (!f) return {};
        std::string line;
        std::getline(f, line);
        // Убираем пробелы и дефисы (UUID формат)
        line.erase(std::remove_if(line.begin(), line.end(),
                                  [](char c){ return c == '-' || c == '\n' || c == '\r' || c == ' '; }),
                   line.end());
        return line;
    }
};

} // namespace gn
