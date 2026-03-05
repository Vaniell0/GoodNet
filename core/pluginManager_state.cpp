#include "pluginManager.hpp"
#include "logger.hpp"
#include <sodium.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <fmt/format.h>

using json = nlohmann::json;

namespace gn {

// ─── SHA-256 через libsodium (потоковый, 64 KB буфер) ────────────────────────

std::string PluginManager::calculate_sha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";

    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);

    std::vector<char> buf(65536);
    while (file.read(buf.data(), buf.size()) || file.gcount() > 0) {
        crypto_hash_sha256_update(&state,
            reinterpret_cast<const unsigned char*>(buf.data()),
            static_cast<unsigned long long>(file.gcount()));
        if (file.eof()) break; // Гарантируем выход
    }

    unsigned char out[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&state, out);

    std::string hex;
    hex.reserve(crypto_hash_sha256_BYTES * 2);
    for (auto b : out)
        fmt::format_to(std::back_inserter(hex), "{:02x}", b);
    return hex;
}

// ─── Верификация JSON-манифеста ───────────────────────────────────────────────
//
// Манифест называется <plugin>.*.json
//
// buildPlugin.nix генерирует: for libfile in *.so → echo > "$libfile.json"
// Т.е.: liblogger.so → liblogger.so.json

std::expected<void, std::string> PluginManager::verify_metadata(const fs::path& so_path) const {
    // Append, не replace_extension
    const fs::path json_path = so_path.string() + ".json";

    if (!fs::exists(json_path))
        return std::unexpected(
            fmt::format("Plugin manifest missing: {}", json_path.filename().string()));

    try {
        std::ifstream f(json_path);
        json data = json::parse(f);

        auto& meta = data.at("meta");
        LOG_DEBUG("Verifying: {} v{}",
                  meta.at("name").get<std::string>(),
                  meta.at("version").get<std::string>());

        const std::string expected = data.at("integrity").at("hash").get<std::string>();
        const std::string actual   = calculate_sha256(so_path);

        if (expected != actual)
            return std::unexpected(fmt::format(
                "Hash mismatch for '{}': expected {}..., got {}...",
                so_path.filename().string(),
                expected.substr(0, 8), actual.substr(0, 8)));

        return {};
    } catch (const std::exception& e) {
        return std::unexpected(fmt::format("Metadata parse error: {}", e.what()));
    }
}

// ─── Управление состоянием ───────────────────────────────────────────────────

bool PluginManager::unload_handler(std::string_view name) {
    std::unique_lock lock(rw_mutex_);
    if (auto it = handlers_.find(name); it != handlers_.end()) {
        LOG_INFO("Unloading handler: '{}'", name);
        handlers_.erase(it);
        return true;
    }
    return false;
}

bool PluginManager::enable_handler(std::string_view name) {
    std::unique_lock lock(rw_mutex_);
    if (auto it = handlers_.find(name); it != handlers_.end()) {
        it->second->enabled = true;
        LOG_INFO("Handler '{}' enabled", name);
        return true;
    }
    return false;
}

bool PluginManager::disable_handler(std::string_view name) {
    std::unique_lock lock(rw_mutex_);
    if (auto it = handlers_.find(name); it != handlers_.end()) {
        it->second->enabled = false;
        LOG_INFO("Handler '{}' disabled", name);
        return true;
    }
    return false;
}

} // namespace gn
