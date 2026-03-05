#include "connectionManager.hpp"
#include "data/machine_id.hpp"
#include "logger.hpp"

#include <fstream>
#include <cstring>
#include <sys/stat.h>

namespace gn {

// ─── Утилиты ──────────────────────────────────────────────────────────────────

std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::string out; out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        char b[3]; std::snprintf(b, 3, "%02x", data[i]); out += b;
    }
    return out;
}

static uint32_t read_u32be(const uint8_t* p) {
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}

// ─── NodeIdentity ─────────────────────────────────────────────────────────────

std::string NodeIdentity::user_pubkey_hex()   const { return bytes_to_hex(user_pubkey,   32); }
std::string NodeIdentity::device_pubkey_hex() const { return bytes_to_hex(device_pubkey, 32); }

void NodeIdentity::save_key(const fs::path& path, const uint8_t* key, size_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write key: " + path.string());
    f.write(reinterpret_cast<const char*>(key), static_cast<std::streamsize>(size));
    f.close();
    ::chmod(path.c_str(), 0600);
}

void NodeIdentity::load_or_gen_keypair(const fs::path& path,
                                        uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                        uint8_t out_sec[crypto_sign_SECRETKEYBYTES]) {
    if (fs::exists(path) && fs::file_size(path) == crypto_sign_SECRETKEYBYTES) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(out_sec), crypto_sign_SECRETKEYBYTES);
            if (static_cast<size_t>(f.gcount()) == crypto_sign_SECRETKEYBYTES) {
                // libsodium хранит seckey как [seed(32) || pubkey(32)]
                std::memcpy(out_pub,
                            out_sec + crypto_sign_SECRETKEYBYTES - crypto_sign_PUBLICKEYBYTES,
                            crypto_sign_PUBLICKEYBYTES);
                LOG_DEBUG("Loaded keypair from '{}'", path.string());
                return;
            }
        }
        LOG_WARN("'{}' unreadable, regenerating", path.string());
    }
    crypto_sign_keypair(out_pub, out_sec);
    save_key(path, out_sec, crypto_sign_SECRETKEYBYTES);
    LOG_INFO("Generated keypair → '{}'", path.string());
}

// ─── OpenSSH Ed25519 private key parser ───────────────────────────────────────
//
// Формат: PEM-обёртка вокруг бинарного блока.
// Спецификация: https://github.com/openssh/openssh-portable/blob/master/PROTOCOL.key

static std::vector<uint8_t> base64_decode(std::string_view in) {
    static constexpr int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out; out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0; int bits = 0;
    for (unsigned char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        int8_t v = T[c]; if (v < 0) continue;
        acc = (acc << 6) | uint8_t(v); bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(uint8_t((acc >> bits) & 0xFF)); }
    }
    return out;
}

bool NodeIdentity::try_load_ssh_key(const fs::path& path,
                                     uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                     uint8_t out_sec[crypto_sign_SECRETKEYBYTES]) {
    if (!fs::exists(path)) return false;
    std::ifstream f(path); if (!f) return false;
    std::string pem((std::istreambuf_iterator<char>(f)), {});

    static constexpr std::string_view BEGIN = "-----BEGIN OPENSSH PRIVATE KEY-----";
    static constexpr std::string_view END   = "-----END OPENSSH PRIVATE KEY-----";
    auto bp = pem.find(BEGIN), ep = pem.find(END);
    if (bp == std::string::npos || ep == std::string::npos) return false;

    const auto raw = base64_decode(
        std::string_view(pem).substr(bp + BEGIN.size(), ep - bp - BEGIN.size()));

    // "openssh-key-v1\0"
    static constexpr uint8_t MAGIC[] =
        {'o','p','e','n','s','s','h','-','k','e','y','-','v','1','\0'};
    if (raw.size() < sizeof(MAGIC) || std::memcmp(raw.data(), MAGIC, sizeof(MAGIC)))
        return false;

    const uint8_t* p = raw.data() + sizeof(MAGIC);
    const uint8_t* E = raw.data() + raw.size();

    // Вспомогательные лямбды для чтения с указателем
    auto skip_str  = [&](std::string* s = nullptr) -> bool {
        if (p + 4 > E) return false;
        uint32_t l = read_u32be(p); p += 4;
        if (p + l > E) return false;
        if (s) *s = std::string(reinterpret_cast<const char*>(p), l);
        p += l; return true;
    };
    auto skip_blob = [&](const uint8_t** bp2 = nullptr, uint32_t* bl = nullptr) -> bool {
        if (p + 4 > E) return false;
        uint32_t l = read_u32be(p); p += 4;
        if (p + l > E) return false;
        if (bp2) *bp2 = p;
        if (bl) *bl = l;
        p += l; return true;
    };

    // cipher, kdfname, kdf options, num_keys
    std::string cipher;
    if (!skip_str(&cipher)) return false;
    if (cipher != "none") {
        LOG_WARN("SSH key '{}': encrypted ({}), skipping", path.string(), cipher);
        return false;
    }
    if (!skip_str() || !skip_blob()) return false;
    if (p + 4 > E || read_u32be(p) == 0) return false;
    p += 4;
    if (!skip_blob()) return false;  // public key blob

    // private key block
    const uint8_t* priv = nullptr; uint32_t priv_len = 0;
    if (!skip_blob(&priv, &priv_len)) return false;

    // Внутри private key block: checkint × 2 + key entries
    const uint8_t* pp = priv, *PE = priv + priv_len;
    if (pp + 8 > PE) return false;
    uint32_t c1 = read_u32be(pp), c2 = read_u32be(pp + 4); pp += 8;
    if (c1 != c2) return false;  // checksum mismatch

    auto skip_str_p  = [&](std::string* s = nullptr) -> bool {
        if (pp + 4 > PE) return false;
        uint32_t l = read_u32be(pp); pp += 4;
        if (pp + l > PE) return false;
        if (s) *s = std::string(reinterpret_cast<const char*>(pp), l);
        pp += l; return true;
    };
    auto read_blob_p = [&](const uint8_t** b, uint32_t* bl) -> bool {
        if (pp + 4 > PE) return false;
        uint32_t l = read_u32be(pp); pp += 4;
        if (pp + l > PE) return false;
        *b = pp; *bl = l; pp += l; return true;
    };

    std::string kt; if (!skip_str_p(&kt)) return false;
    if (kt != "ssh-ed25519") {
        LOG_WARN("SSH key '{}': type '{}', need ed25519", path.string(), kt);
        return false;
    }

    const uint8_t* pub_b = nullptr; uint32_t pub_l = 0;
    if (!read_blob_p(&pub_b, &pub_l) || pub_l != 32) return false;
    const uint8_t* sec_b = nullptr; uint32_t sec_l = 0;
    if (!read_blob_p(&sec_b, &sec_l) || sec_l != 64) return false;

    std::memcpy(out_pub, pub_b, 32);
    std::memcpy(out_sec, sec_b, 64);
    LOG_INFO("Loaded user key from SSH: '{}'", path.string());
    return true;
}

// ─── load_or_generate ─────────────────────────────────────────────────────────

NodeIdentity NodeIdentity::load_or_generate(const IdentityConfig& cfg) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium init failed");
    fs::create_directories(cfg.dir);

    NodeIdentity id{};

    // user_key: config.ssh_key → ~/.ssh/id_ed25519 → сгенерировать
    bool loaded = false;
    if (!cfg.ssh_key_path.empty())
        loaded = try_load_ssh_key(cfg.ssh_key_path, id.user_pubkey, id.user_seckey);
    if (!loaded) {
        const char* home = std::getenv("HOME");
        loaded = try_load_ssh_key(
            fs::path(home ? home : ".") / ".ssh" / "id_ed25519",
            id.user_pubkey, id.user_seckey);
    }
    if (!loaded)
        load_or_gen_keypair(cfg.dir / "user_key", id.user_pubkey, id.user_seckey);

    // device_key: BLAKE2b(machine-id || user_pubkey) → сгенерировать
    if (cfg.use_machine_id) {
        const std::string mid = MachineId::get_or_create(cfg.dir);
        if (!mid.empty()) {
            MachineId::derive_device_keypair(mid, id.user_pubkey,
                                              id.device_pubkey, id.device_seckey);
            LOG_INFO("Device key: hardware-bound (machine-id)");
        } else {
            load_or_gen_keypair(cfg.dir / "device_key", id.device_pubkey, id.device_seckey);
        }
    } else {
        load_or_gen_keypair(cfg.dir / "device_key", id.device_pubkey, id.device_seckey);
    }

    LOG_INFO("Identity ready — user={}...  device={}...",
             bytes_to_hex(id.user_pubkey, 4), bytes_to_hex(id.device_pubkey, 4));
    return id;
}

} // namespace gn
