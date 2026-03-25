#pragma once

#include <filesystem>
#include <string>

#include <sodium/crypto_sign.h>

namespace gn {

namespace fs = std::filesystem;

// ── IdentityConfig ────────────────────────────────────────────────────────────

struct IdentityConfig {
    fs::path dir            = "~/.goodnet";
    fs::path ssh_key_path   = {};
    bool     use_machine_id = true;
    bool     skip_ssh_fallback = false;  ///< Не пробовать ~/.ssh/id_ed25519
};

// ── NodeIdentity ──────────────────────────────────────────────────────────────

/// Ed25519 user + device keypair for a node instance.
struct NodeIdentity {
    uint8_t user_pubkey  [crypto_sign_PUBLICKEYBYTES]{};
    uint8_t user_seckey  [crypto_sign_SECRETKEYBYTES]{};
    uint8_t device_pubkey[crypto_sign_PUBLICKEYBYTES]{};
    uint8_t device_seckey[crypto_sign_SECRETKEYBYTES]{};

    static NodeIdentity load_or_generate(const IdentityConfig& cfg);
    static NodeIdentity load_or_generate(const fs::path& dir) {
        return load_or_generate(IdentityConfig{.dir = dir, .skip_ssh_fallback = true});
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
