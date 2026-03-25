#include "connectionManager.hpp"
#include "data/machine_id.hpp"
#include "logger.hpp"

#include <fstream>
#include <cstring>

#include <sodium.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "util.hpp"

namespace gn {

static uint32_t read_u32be(const uint8_t* p) {
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}

std::string NodeIdentity::user_pubkey_hex()   const { return bytes_to_hex(user_pubkey,   crypto_sign_PUBLICKEYBYTES); }
std::string NodeIdentity::device_pubkey_hex() const { return bytes_to_hex(device_pubkey, crypto_sign_PUBLICKEYBYTES); }

void NodeIdentity::save_key(const fs::path& path, const uint8_t* key, size_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write key: " + path.string());
    f.write(reinterpret_cast<const char*>(key), static_cast<std::streamsize>(size));
    f.close();
#if !defined(_WIN32)
    ::chmod(path.c_str(), 0600);
#endif
}

void NodeIdentity::load_or_gen_keypair(const fs::path& path,
                                        uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                        uint8_t out_sec[crypto_sign_SECRETKEYBYTES]) {
    if (fs::exists(path) && fs::file_size(path) == crypto_sign_SECRETKEYBYTES) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(out_sec), crypto_sign_SECRETKEYBYTES);
            if (static_cast<size_t>(f.gcount()) == crypto_sign_SECRETKEYBYTES) {
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

static std::vector<uint8_t> base64_decode(std::string_view in) {
    std::vector<uint8_t> out(in.size()); 
    size_t bin_len;
    
    // Добавляем "\n\r " в параметр ignore (предпоследний аргумент)
    if (sodium_base642bin(out.data(), out.size(), in.data(), in.size(),
                          "\n\r ", &bin_len, nullptr, 
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return {}; 
    } out.resize(bin_len);
    return out;
}

bool NodeIdentity::try_load_ssh_key(const fs::path& path,
                                     uint8_t out_pub[crypto_sign_PUBLICKEYBYTES],
                                     uint8_t out_sec[crypto_sign_SECRETKEYBYTES]) {
    if (!fs::exists(path)) return false;
    std::ifstream f(path); if (!f) return false;
    std::string pem((std::istreambuf_iterator<char>(f)), {});

    static constexpr std::string_view BEGIN = "-----BEGIN OPENSSH PRIVATE KEY-----";
    static constexpr std::string_view END   =  "-----END OPENSSH PRIVATE KEY-----";
    auto bp = pem.find(BEGIN), ep = pem.find(END);
    if (bp == std::string::npos || ep == std::string::npos) return false;

    const auto raw = base64_decode(
        std::string_view(pem).substr(bp + BEGIN.size(), ep - bp - BEGIN.size()));

    static constexpr uint8_t MAGIC[] =
        {'o','p','e','n','s','s','h','-','k','e','y','-','v','1','\0'};
    if (raw.size() < sizeof(MAGIC) || std::memcmp(raw.data(), MAGIC, sizeof(MAGIC)))
        return false;

    const uint8_t* p = raw.data() + sizeof(MAGIC);
    const uint8_t* E = raw.data() + raw.size();

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
        if (bl)  *bl  = l;
        p += l; return true;
    };

    std::string cipher;
    if (!skip_str(&cipher)) return false;
    if (cipher != "none") {
        LOG_WARN("SSH key '{}': encrypted ({}), skipping", path.string(), cipher);
        return false;
    }
    if (!skip_str() || !skip_blob()) return false;
    if (p + 4 > E || read_u32be(p) == 0) return false;
    p += 4;
    if (!skip_blob()) return false;

    const uint8_t* priv = nullptr; uint32_t priv_len = 0;
    if (!skip_blob(&priv, &priv_len)) return false;

    const uint8_t* pp = priv; const uint8_t* PE = priv + priv_len;
    if (pp + 8 > PE) return false;
    uint32_t c1 = read_u32be(pp), c2 = read_u32be(pp + 4); pp += 8;
    if (c1 != c2) return false;

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
    if (!read_blob_p(&pub_b, &pub_l) || pub_l != crypto_sign_PUBLICKEYBYTES) return false;
    const uint8_t* sec_b = nullptr; uint32_t sec_l = 0;
    if (!read_blob_p(&sec_b, &sec_l) || sec_l != crypto_sign_SECRETKEYBYTES) return false;

    std::memcpy(out_pub, pub_b, crypto_sign_PUBLICKEYBYTES);
    std::memcpy(out_sec, sec_b, crypto_sign_SECRETKEYBYTES);
    LOG_INFO("Loaded user key from SSH: '{}'", path.string());
    return true;
}

NodeIdentity NodeIdentity::load_or_generate(const IdentityConfig& cfg) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium init failed");
    fs::create_directories(cfg.dir);

    NodeIdentity id{};

    bool loaded = false;
    if (!cfg.ssh_key_path.empty())
        loaded = try_load_ssh_key(cfg.ssh_key_path, id.user_pubkey, id.user_seckey);
    if (!loaded && !cfg.skip_ssh_fallback) {
        const char* home = std::getenv("HOME");
#if defined(_WIN32)
        if (!home) home = std::getenv("USERPROFILE");
#endif
        loaded = try_load_ssh_key(
            fs::path(home ? home : ".") / ".ssh" / "id_ed25519",
            id.user_pubkey, id.user_seckey);
    }
    if (!loaded)
        load_or_gen_keypair(cfg.dir / "user_key", id.user_pubkey, id.user_seckey);

    if (cfg.use_machine_id) {
        const std::string mid = MachineId::get_or_create(cfg.dir);
        if (!mid.empty()) {
            MachineId::derive_device_keypair(mid, id.user_pubkey,
                                              id.device_pubkey, id.device_seckey);
            LOG_INFO("Device key: hardware-bound");
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
