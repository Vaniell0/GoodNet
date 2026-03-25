#pragma once

#include <filesystem>
#include <string>

#include <sodium/crypto_sign.h>

#include "config.hpp"

namespace gn {

namespace fs = std::filesystem;

// ── NodeIdentity ──────────────────────────────────────────────────────────────

/// Ed25519 user + device keypair for a node instance.
struct NodeIdentity {
    uint8_t user_pubkey  [crypto_sign_PUBLICKEYBYTES]{};
    uint8_t user_seckey  [crypto_sign_SECRETKEYBYTES]{};
    uint8_t device_pubkey[crypto_sign_PUBLICKEYBYTES]{};
    uint8_t device_seckey[crypto_sign_SECRETKEYBYTES]{};

    static NodeIdentity load_or_generate(const Config::Identity& cfg);
    static NodeIdentity load_or_generate(const fs::path& dir) {
        Config::Identity cfg;
        cfg.dir = dir.string();
        cfg.skip_ssh_fallback = true;
        return load_or_generate(cfg);
    }

    [[nodiscard]] std::string user_pubkey_hex()   const;
    [[nodiscard]] std::string device_pubkey_hex() const;

    static bool try_load_ssh_key(const fs::path& path,
                                  uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                  uint8_t out_sec[crypto_sign_SECRETKEYBYTES]);
private:
    static void load_or_gen_keypair(const fs::path& path,
                                     uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                     uint8_t out_sec[crypto_sign_SECRETKEYBYTES]);
    static void save_key(const fs::path& path, const uint8_t* key, size_t size);
};

} // namespace gn
