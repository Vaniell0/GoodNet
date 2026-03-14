#pragma once

#include <filesystem>
#include <string>

namespace gn {

namespace fs = std::filesystem;

// ── IdentityConfig ────────────────────────────────────────────────────────────

struct IdentityConfig {
    fs::path dir            = "~/.goodnet";
    fs::path ssh_key_path;
    bool     use_machine_id = true;
};

// ── NodeIdentity ──────────────────────────────────────────────────────────────

/// Ed25519 user + device keypair for a node instance.
struct NodeIdentity {
    uint8_t user_pubkey  [32]{};
    uint8_t user_seckey  [64]{};
    uint8_t device_pubkey[32]{};
    uint8_t device_seckey[64]{};

    static NodeIdentity load_or_generate(const IdentityConfig& cfg);
    static NodeIdentity load_or_generate(const fs::path& dir) {
        return load_or_generate(IdentityConfig{.dir = dir});
    }

    [[nodiscard]] std::string user_pubkey_hex()   const;
    [[nodiscard]] std::string device_pubkey_hex() const;

    static bool try_load_ssh_key(const fs::path& path,
                                  uint8_t out_pub[32],
                                  uint8_t out_sec[64]);
private:
    static void load_or_gen_keypair(const fs::path& path,
                                     uint8_t out_pub[32],
                                     uint8_t out_sec[64]);
    static void save_key(const fs::path& path, const uint8_t* key, size_t size);
};

} // namespace gn
