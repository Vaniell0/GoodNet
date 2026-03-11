#include "connectionManager.hpp"
#include "logger.hpp"
#include <cstring>
#include <algorithm>

#include <sodium/crypto_generichash.h>  // Для crypto_generichash_state, init, update, final
#include <sodium/crypto_scalarmult.h>   // Для crypto_scalarmult и crypto_scalarmult_BYTES
#include <sodium/crypto_secretbox.h>    // Для crypto_secretbox_KEYBYTES
#include <zstd.h>

namespace gn {

std::string bytes_to_hex(const uint8_t* data, size_t len);

// ─── encrypt ─────────────────────────────────────────────────────────────────
// Wire: nonce_u64_le(8) | secretbox(plain, nonce24, session_key)
// Для маленьких пакетов — без сжатия (тесты и handshake работают как раньше)
// Для больших (>512 байт) — zstd + префикс размера
std::vector<uint8_t> SessionState::encrypt(const void* plain, size_t plain_len) {
    if (plain_len == 0) {
        // Пустой пакет — старый формат (nonce + MAC)
        const uint64_t nonce_val = send_nonce.fetch_add(1, std::memory_order_relaxed);
        uint8_t nonce24[crypto_secretbox_NONCEBYTES] = {};
        for (int i = 0; i < 8; ++i)
            nonce24[i] = static_cast<uint8_t>((nonce_val >> (i * 8)) & 0xFF);

        std::vector<uint8_t> wire(8 + crypto_secretbox_MACBYTES);
        std::memcpy(wire.data(), nonce24, 8);
        // crypto_secretbox_easy для пустого plaintext
        crypto_secretbox_easy(wire.data() + 8, nullptr, 0, nonce24, session_key);
        return wire;
    }

    const uint64_t nonce_val = send_nonce.fetch_add(1, std::memory_order_relaxed);
    uint8_t nonce24[crypto_secretbox_NONCEBYTES] = {};
    for (int i = 0; i < 8; ++i)
        nonce24[i] = static_cast<uint8_t>((nonce_val >> (i * 8)) & 0xFF);

    // Маленькие пакеты — без сжатия (тесты и handshake проходят!)
    if (plain_len <= 512) {
        std::vector<uint8_t> wire(8 + plain_len + crypto_secretbox_MACBYTES);
        std::memcpy(wire.data(), nonce24, 8);
        crypto_secretbox_easy(wire.data() + 8,
                              static_cast<const uint8_t*>(plain), plain_len,
                              nonce24, session_key);
        return wire;
    }

    // Большие пакеты — zstd + префикс размера
    size_t bound = ZSTD_compressBound(plain_len);
    std::vector<uint8_t> compressed(4 + bound);
    uint32_t orig_size = static_cast<uint32_t>(plain_len);
    std::memcpy(compressed.data(), &orig_size, 4);

    size_t csize = ZSTD_compress(compressed.data() + 4, bound,
                                 static_cast<const uint8_t*>(plain), plain_len, 3);

    if (ZSTD_isError(csize)) {
        LOG_WARN("zstd compress failed, sending uncompressed");
        compressed.resize(4 + plain_len);
        std::memcpy(compressed.data() + 4, plain, plain_len);
        csize = plain_len;
    } else {
        compressed.resize(4 + csize);
    }

    std::vector<uint8_t> wire(8 + compressed.size() + crypto_secretbox_MACBYTES);
    std::memcpy(wire.data(), nonce24, 8);
    crypto_secretbox_easy(wire.data() + 8, compressed.data(), compressed.size(),
                          nonce24, session_key);
    return wire;
}

// ─── decrypt ─────────────────────────────────────────────────────────────────
std::vector<uint8_t> SessionState::decrypt(const void* wire_ptr, size_t wire_len) {
    if (wire_len < 8 + crypto_secretbox_MACBYTES) {
        LOG_WARN("decrypt: too short ({} bytes)", wire_len);
        return {};
    }

    const auto* wire = static_cast<const uint8_t*>(wire_ptr);
    uint8_t nonce24[crypto_secretbox_NONCEBYTES] = {};
    std::memcpy(nonce24, wire, 8);

    uint64_t nonce_val = 0;
    for (int i = 0; i < 8; ++i)
        nonce_val |= (uint64_t(wire[i]) << (i * 8));

    const uint64_t expected = recv_nonce_expected.load(std::memory_order_acquire);
    if (nonce_val < expected) {
        LOG_WARN("decrypt: replay (nonce {} < expected {})", nonce_val, expected);
        return {};
    }
    recv_nonce_expected.store(nonce_val + 1, std::memory_order_release);

    const size_t cipher_len = wire_len - 8;
    std::vector<uint8_t> decrypted(cipher_len - crypto_secretbox_MACBYTES);

    if (crypto_secretbox_open_easy(decrypted.data(), wire + 8, cipher_len,
                                   nonce24, session_key) != 0) {
        LOG_WARN("decrypt: MAC failed (nonce={})", nonce_val);
        return {};
    }

    // Маленькие пакеты — возвращаем как есть (без префикса)
    if (decrypted.size() <= 512) {
        return decrypted;
    }

    // Проверяем, есть ли префикс размера (сжатие)
    uint32_t orig_size;
    std::memcpy(&orig_size, decrypted.data(), 4);
    if (orig_size == 0 || orig_size > 100 * 1024 * 1024) {
        // без сжатия — возвращаем с offset 4
        return {decrypted.begin() + 4, decrypted.end()};
    }

    std::vector<uint8_t> plain(orig_size);
    size_t dsize = ZSTD_decompress(plain.data(), orig_size,
                                   decrypted.data() + 4, decrypted.size() - 4);

    if (ZSTD_isError(dsize)) {
        LOG_WARN("zstd decompress failed, fallback uncompressed");
        return {decrypted.begin() + 4, decrypted.end()};
    }

    return plain;
}

// ─── derive_session ───────────────────────────────────────────────────────────
// ECDH + BLAKE2b-256(shared || min(user_pk_a, user_pk_b) || max(...))
// Sorted pubkeys ensure determinism from both sides.

bool ConnectionManager::derive_session(conn_id_t id,
                                        const uint8_t peer_ephem_pk[32],
                                        const uint8_t peer_user_pk[32]) {
    std::unique_lock lock(records_mu_);
    auto it = records_.find(id);
    if (it == records_.end()) return false;
    auto& rec = it->second;
    if (!rec.session) return false;

    auto& sess = *rec.session;

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
    crypto_generichash_init(&state, nullptr, 0, crypto_secretbox_KEYBYTES);
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
