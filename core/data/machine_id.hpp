#pragma once

#include <string>
#include <string_view>
#include <filesystem>

namespace gn {

namespace fs = std::filesystem;

class MachineId {
public:
    /// @brief Собрать все аппаратные ID и вернуть хеш.
    static std::string read();

    /// @brief Получить ID или создать случайный в указанной директории.
    static std::string get_or_create(const fs::path& dir);

    /// @brief Деривация ключей устройства.
    static void derive_device_keypair(
        std::string_view machine_id,
        const uint8_t   user_pubkey[32],
        uint8_t         out_pub[32],
        uint8_t         out_sec[64]);
};

} // namespace gn
