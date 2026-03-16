/// @file core/cm_session.cpp
/// Encryption, decryption, and session key derivation.

#include "connectionManager.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cstring>

#include <sodium/crypto_generichash.h>
#include <sodium/crypto_scalarmult.h>
#include <sodium/crypto_secretbox.h>
#include <zstd.h>

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

// ── Wire flags ────────────────────────────────────────────────────────────────

static constexpr uint8_t FLAG_RAW  = 0x00;
static constexpr uint8_t FLAG_ZSTD = 0x01;

// ── encrypt ───────────────────────────────────────────────────────────────────

std::vector<uint8_t> SessionState::encrypt(const void* plain, size_t plain_len) {
    const uint64_t nonce_val = send_nonce.fetch_add(1, std::memory_order_relaxed);
    uint8_t nonce24[crypto_secretbox_NONCEBYTES]{};
    for (int i = 0; i < 8; ++i)
        nonce24[i] = static_cast<uint8_t>((nonce_val >> (i * 8)) & 0xFF);

    bool compressed = false;
    std::vector<uint8_t> comp_buf;

    if (plain_len > 512) {
        const size_t bound = ZSTD_compressBound(plain_len);
        comp_buf.resize(bound);
        const size_t csize = ZSTD_compress(comp_buf.data(), bound,
                                            static_cast<const uint8_t*>(plain),
                                            plain_len, 1);
        if (!ZSTD_isError(csize) && csize < plain_len) {
            comp_buf.resize(csize);
            compressed = true;
        }
    }

    std::vector<uint8_t> body;
    if (compressed) {
        const uint32_t orig32 = static_cast<uint32_t>(plain_len);
        body.resize(1 + 4 + comp_buf.size());
        body[0] = FLAG_ZSTD;
        std::memcpy(body.data() + 1, &orig32, 4);
        std::memcpy(body.data() + 5, comp_buf.data(), comp_buf.size());
    } else {
        body.resize(1 + plain_len);
        body[0] = FLAG_RAW;
        if (plain_len && plain)
            std::memcpy(body.data() + 1, plain, plain_len);
    }

    std::vector<uint8_t> wire(8 + body.size() + crypto_secretbox_MACBYTES);
    std::memcpy(wire.data(), nonce24, 8);
    crypto_secretbox_easy(wire.data() + 8, body.data(), body.size(),
                          nonce24, session_key);
    return wire;
}

// ── decrypt ───────────────────────────────────────────────────────────────────

std::vector<uint8_t> SessionState::decrypt(const void* wire_ptr, size_t wire_len) {
    if (wire_len < 8 + crypto_secretbox_MACBYTES) {
        LOG_WARN("decrypt: too short ({} bytes)", wire_len);
        return {};
    }

    const auto* wire = static_cast<const uint8_t*>(wire_ptr);
    uint8_t nonce24[crypto_secretbox_NONCEBYTES]{};
    std::memcpy(nonce24, wire, 8);

    uint64_t nonce_val = 0;
    for (int i = 0; i < 8; ++i)
        nonce_val |= (uint64_t(wire[i]) << (i * 8));

    // CAS loop: thread-safe nonce validation without TOCTOU race.
    {
        uint64_t expected = recv_nonce_expected.load(std::memory_order_acquire);
        while (true) {
            if (nonce_val < expected) {
                LOG_WARN("decrypt: replay (nonce={} < expected={})", nonce_val, expected);
                return {};
            }
            if (recv_nonce_expected.compare_exchange_weak(expected, nonce_val + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                break;
            // expected обновился — проверяем снова
        }
    }

    const size_t cipher_len = wire_len - 8;
    std::vector<uint8_t> body(cipher_len - crypto_secretbox_MACBYTES);

    if (crypto_secretbox_open_easy(body.data(), wire + 8, cipher_len,
                                    nonce24, session_key) != 0) {
        LOG_WARN("decrypt: MAC failed (nonce={})", nonce_val);
        return {};
    }

    if (body.empty()) { LOG_WARN("decrypt: empty body"); return {}; }

    const uint8_t  flags   = body[0];
    const uint8_t* payload = body.data() + 1;
    const size_t   plen    = body.size() - 1;

    if (flags == FLAG_RAW)
        return std::vector<uint8_t>(payload, payload + plen);

    if (flags == FLAG_ZSTD) {
        if (plen < 4) { LOG_WARN("decrypt: no orig_size"); return {}; }
        uint32_t orig_size = 0;
        std::memcpy(&orig_size, payload, 4);
        if (!orig_size || orig_size > 128 * 1024 * 1024) {
            LOG_WARN("decrypt: implausible orig_size={}", orig_size);
            return {};
        }
        std::vector<uint8_t> plain(orig_size);
        const size_t dsize = ZSTD_decompress(plain.data(), orig_size,
                                              payload + 4, plen - 4);
        if (ZSTD_isError(dsize)) {
            LOG_WARN("decrypt: ZSTD error: {}", ZSTD_getErrorName(dsize));
            return {};
        }
        plain.resize(dsize);
        return plain;
    }

    LOG_WARN("decrypt: unknown flags 0x{:02X}", flags);
    return {};
}

// ── derive_session ────────────────────────────────────────────────────────────
// ECDH + BLAKE2b-256(shared || sorted(user_pk_a, user_pk_b))

bool ConnectionManager::derive_session(conn_id_t id,
                                        const uint8_t peer_ephem_pk[32],
                                        const uint8_t peer_user_pk [32]) {
    // Read session pointer without write lock (session is only written here
    // and in handle_connect under records_write_mu_, which serialises writers).
    auto rec = rcu_find(id);
    if (!rec || !rec->session) return false;

    auto& sess = *rec->session;

    uint8_t shared[crypto_scalarmult_BYTES]{};
    if (crypto_scalarmult(shared, sess.my_ephem_sk, peer_ephem_pk) != 0) {
        LOG_ERROR("derive_session #{}: ECDH failed", id);
        sodium_memzero(shared, sizeof(shared));
        return false;
    }

    const uint8_t* pk_a = identity_.user_pubkey;
    const uint8_t* pk_b = peer_user_pk;
    if (std::memcmp(pk_a, pk_b, 32) > 0) std::swap(pk_a, pk_b);

    static_assert(crypto_secretbox_KEYBYTES == crypto_generichash_BYTES);
    crypto_generichash_state state;
    crypto_generichash_init (&state, nullptr, 0, crypto_secretbox_KEYBYTES);
    crypto_generichash_update(&state, shared, sizeof(shared));
    crypto_generichash_update(&state, pk_a,   32);
    crypto_generichash_update(&state, pk_b,   32);
    crypto_generichash_final (&state, sess.session_key, sizeof(sess.session_key));

    sodium_memzero(shared, sizeof(shared));
    sess.clear_ephemeral();
    sess.ready = true;

    LOG_INFO("Session #{}: key={}...", id, bytes_to_hex(sess.session_key, 4));
    return true;
}

} // namespace gn
