/// @file core/noise.cpp
/// Noise_XX_25519_ChaChaPoly_BLAKE2b — реализация на libsodium.
///
/// Криптопримитивы:
///   DH:   crypto_scalarmult (X25519)
///   AEAD: crypto_aead_chacha20poly1305_ietf
///   Hash: crypto_generichash_blake2b (HASHLEN=32)
///   HKDF: BLAKE2b keyed mode (эквивалент HMAC для PRF-secure hash)

#include "noise.hpp"

#include <cstring>

#include <sodium/crypto_aead_chacha20poly1305.h>
#include <sodium/crypto_generichash.h>
#include <sodium/crypto_scalarmult.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>

namespace gn::noise {

// ── Вспомогательные криптофункции ────────────────────────────────────────────

bool dh(uint8_t out[DHLEN], const uint8_t sk[DHLEN], const uint8_t pk[DHLEN]) {
    return crypto_scalarmult(out, sk, pk) == 0;
}

void generate_keypair(uint8_t pk[DHLEN], uint8_t sk[DHLEN]) {
    randombytes_buf(sk, DHLEN);
    crypto_scalarmult_base(pk, sk);
}

void hash(uint8_t out[HASHLEN], const uint8_t* data, size_t len) {
    crypto_generichash_blake2b(out, HASHLEN, data, len, nullptr, 0);
}

void hmac_hash(uint8_t out[HASHLEN],
               const uint8_t key[HASHLEN],
               const uint8_t* data, size_t data_len) {
    // BLAKE2b keyed mode — безопасная PRF, эквивалент HMAC для HKDF.
    // libsodium корректно обрабатывает data=NULL при data_len=0.
    crypto_generichash_blake2b(out, HASHLEN, data, data_len, key, HASHLEN);
}

void hkdf2(const uint8_t ck[HASHLEN],
           const uint8_t* ikm, size_t ikm_len,
           uint8_t out1[HASHLEN], uint8_t out2[HASHLEN]) {
    // temp_key = HMAC(ck, ikm)
    uint8_t temp_key[HASHLEN];
    hmac_hash(temp_key, ck, ikm, ikm_len);

    // output1 = HMAC(temp_key, 0x01)
    const uint8_t one = 0x01;
    hmac_hash(out1, temp_key, &one, 1);

    // output2 = HMAC(temp_key, output1 || 0x02)
    uint8_t buf[HASHLEN + 1];
    std::memcpy(buf, out1, HASHLEN);
    buf[HASHLEN] = 0x02;
    hmac_hash(out2, temp_key, buf, sizeof(buf));

    sodium_memzero(temp_key, sizeof(temp_key));
    sodium_memzero(buf, sizeof(buf));
}

// ── CipherState ──────────────────────────────────────────────────────────────

bool CipherState::encrypt(const uint8_t* ad, size_t ad_len,
                           const uint8_t* plain, size_t plain_len,
                           uint8_t* out, size_t* out_len) {
    // Если ключ не установлен — passthrough (msg1 payload)
    if (!has_key) {
        if (plain_len > 0)
            std::memcpy(out, plain, plain_len);
        *out_len = plain_len;
        return true;
    }

    // Nonce: 4 zero bytes + 8-byte LE counter (Noise spec для ChaChaPoly)
    uint8_t nonce12[NONCELEN]{};
    std::memcpy(nonce12 + 4, &nonce, 8);

    unsigned long long clen = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            out, &clen,
            plain, plain_len,
            ad, ad_len,
            nullptr, nonce12, key) != 0)
        return false;

    *out_len = static_cast<size_t>(clen);
    ++nonce;
    return true;
}

bool CipherState::decrypt(const uint8_t* ad, size_t ad_len,
                           const uint8_t* cipher, size_t cipher_len,
                           uint8_t* out, size_t* out_len) {
    if (!has_key) {
        if (cipher_len > 0)
            std::memcpy(out, cipher, cipher_len);
        *out_len = cipher_len;
        return true;
    }

    if (cipher_len < MACLEN) return false;

    uint8_t nonce12[NONCELEN]{};
    std::memcpy(nonce12 + 4, &nonce, 8);

    unsigned long long mlen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            out, &mlen, nullptr,
            cipher, cipher_len,
            ad, ad_len,
            nonce12, key) != 0)
        return false;

    *out_len = static_cast<size_t>(mlen);
    ++nonce;
    return true;
}

void CipherState::rekey() {
    // REKEY(k) = ENCRYPT(k, 2^64-1, "", ZEROS(32))[0:32]
    uint8_t nonce12[NONCELEN]{};
    uint64_t max_nonce = UINT64_MAX;
    std::memcpy(nonce12 + 4, &max_nonce, 8);

    uint8_t zeros[KEYLEN]{};
    uint8_t out[KEYLEN + MACLEN];
    unsigned long long clen = 0;

    crypto_aead_chacha20poly1305_ietf_encrypt(
        out, &clen, zeros, KEYLEN,
        nullptr, 0, nullptr, nonce12, key);

    // Берём первые 32 байта (ciphertext без тега)
    std::memcpy(key, out, KEYLEN);
    sodium_memzero(out, sizeof(out));
    // nonce НЕ сбрасывается при rekey
}

void CipherState::clear() {
    sodium_memzero(key, sizeof(key));
    nonce   = 0;
    has_key = false;
}

// ── SymmetricState ───────────────────────────────────────────────────────────

void SymmetricState::init(const char* protocol_name) {
    const size_t name_len = std::strlen(protocol_name);

    if (name_len <= HASHLEN) {
        // Дополняем нулями до HASHLEN
        std::memset(h, 0, HASHLEN);
        std::memcpy(h, protocol_name, name_len);
    } else {
        // Хешируем если имя длиннее HASHLEN
        hash(h, reinterpret_cast<const uint8_t*>(protocol_name), name_len);
    }

    std::memcpy(ck, h, HASHLEN);
    cipher = CipherState{};
}

void SymmetricState::mix_hash(const uint8_t* data, size_t len) {
    // h = HASH(h || data)
    crypto_generichash_blake2b_state st;
    crypto_generichash_blake2b_init(&st, nullptr, 0, HASHLEN);
    crypto_generichash_blake2b_update(&st, h, HASHLEN);
    crypto_generichash_blake2b_update(&st, data, len);
    crypto_generichash_blake2b_final(&st, h, HASHLEN);
}

void SymmetricState::mix_key(const uint8_t* ikm, size_t len) {
    // ck, temp_k = HKDF(ck, ikm)
    uint8_t temp_k[HASHLEN];
    hkdf2(ck, ikm, len, ck, temp_k);

    // CipherState ← новый ключ, nonce=0
    std::memcpy(cipher.key, temp_k, KEYLEN);
    cipher.nonce   = 0;
    cipher.has_key = true;

    sodium_memzero(temp_k, sizeof(temp_k));
}

bool SymmetricState::encrypt_and_hash(const uint8_t* plain, size_t plain_len,
                                       uint8_t* out, size_t* out_len) {
    // ciphertext = ENCRYPT(k, n, h, plain)  [или passthrough если нет ключа]
    if (!cipher.encrypt(h, HASHLEN, plain, plain_len, out, out_len))
        return false;
    // h = HASH(h || ciphertext)
    mix_hash(out, *out_len);
    return true;
}

bool SymmetricState::decrypt_and_hash(const uint8_t* ciphertext, size_t cipher_len,
                                       uint8_t* out, size_t* out_len) {
    // plaintext = DECRYPT(k, n, h, ciphertext)
    // ВАЖНО: сначала decrypt с текущим h как AD, потом mix_hash(ciphertext)
    if (!cipher.decrypt(h, HASHLEN, ciphertext, cipher_len, out, out_len))
        return false;
    // h = HASH(h || ciphertext)
    mix_hash(ciphertext, cipher_len);
    return true;
}

void SymmetricState::split(CipherState& c1, CipherState& c2) {
    // temp_k1, temp_k2 = HKDF(ck, "", 2)
    uint8_t temp_k1[HASHLEN], temp_k2[HASHLEN];
    hkdf2(ck, nullptr, 0, temp_k1, temp_k2);

    c1 = CipherState{};
    std::memcpy(c1.key, temp_k1, KEYLEN);
    c1.has_key = true;

    c2 = CipherState{};
    std::memcpy(c2.key, temp_k2, KEYLEN);
    c2.has_key = true;

    sodium_memzero(temp_k1, sizeof(temp_k1));
    sodium_memzero(temp_k2, sizeof(temp_k2));
}

// ── HandshakeState ───────────────────────────────────────────────────────────

void HandshakeState::init(bool is_initiator,
                           const uint8_t static_pk[DHLEN],
                           const uint8_t static_sk[DHLEN]) {
    symmetric.init(PROTOCOL_NAME);
    std::memcpy(s_pk, static_pk, DHLEN);
    std::memcpy(s_sk, static_sk, DHLEN);
    initiator = is_initiator;
    step      = 0;
    // XX: нет pre-messages
}

bool HandshakeState::write_message(const uint8_t* payload, size_t payload_len,
                                    uint8_t* out, size_t* out_len) {
    size_t offset = 0;
    uint8_t dh_out[DHLEN];

    switch (step) {
    case 0: {
        // ══ msg1: → e ════════════════════════════════════════════════════
        generate_keypair(e_pk, e_sk);

        // Записываем ephemeral pk в cleartext
        std::memcpy(out, e_pk, DHLEN);
        symmetric.mix_hash(e_pk, DHLEN);
        offset = DHLEN;

        // Payload (без шифрования — cipher ещё без ключа)
        size_t plen;
        if (!symmetric.encrypt_and_hash(payload, payload_len,
                                         out + offset, &plen))
            return false;
        offset += plen;
        break;
    }

    case 1: {
        // ══ msg2: ← e, ee, s, es ════════════════════════════════════════
        generate_keypair(e_pk, e_sk);

        // e: cleartext ephemeral pk
        std::memcpy(out, e_pk, DHLEN);
        symmetric.mix_hash(e_pk, DHLEN);
        offset = DHLEN;

        // ee: DH(e_sk, re)
        if (!dh(dh_out, e_sk, re)) return false;
        symmetric.mix_key(dh_out, DHLEN);

        // s: encrypt_and_hash(static pk)
        size_t slen;
        if (!symmetric.encrypt_and_hash(s_pk, DHLEN, out + offset, &slen))
            return false;
        offset += slen;

        // es: DH(initiator_e, responder_s)
        //   responder пишет msg2 → DH(s_sk, re)
        //   initiator пишет msg2 → невозможно в XX, но для корректности:
        if (initiator)
            { if (!dh(dh_out, e_sk, rs)) return false; }
        else
            { if (!dh(dh_out, s_sk, re)) return false; }
        symmetric.mix_key(dh_out, DHLEN);

        // Payload
        size_t plen;
        if (!symmetric.encrypt_and_hash(payload, payload_len,
                                         out + offset, &plen))
            return false;
        offset += plen;
        break;
    }

    case 2: {
        // ══ msg3: → s, se ════════════════════════════════════════════════

        // s: encrypt_and_hash(static pk)
        size_t slen;
        if (!symmetric.encrypt_and_hash(s_pk, DHLEN, out + offset, &slen))
            return false;
        offset += slen;

        // se: DH(initiator_s, responder_e)
        //   initiator пишет msg3 → DH(s_sk, re)
        //   responder пишет msg3 → невозможно в XX
        if (initiator)
            { if (!dh(dh_out, s_sk, re)) return false; }
        else
            { if (!dh(dh_out, e_sk, rs)) return false; }
        symmetric.mix_key(dh_out, DHLEN);

        // Payload
        size_t plen;
        if (!symmetric.encrypt_and_hash(payload, payload_len,
                                         out + offset, &plen))
            return false;
        offset += plen;
        break;
    }

    default:
        return false;
    }

    sodium_memzero(dh_out, sizeof(dh_out));
    *out_len = offset;
    ++step;
    return true;
}

bool HandshakeState::read_message(const uint8_t* msg, size_t msg_len,
                                   uint8_t* payload_out, size_t* payload_len) {
    size_t offset = 0;
    uint8_t dh_out[DHLEN];

    switch (step) {
    case 0: {
        // ══ msg1: → e ════════════════════════════════════════════════════
        if (msg_len < DHLEN) return false;

        // e: read remote ephemeral
        std::memcpy(re, msg, DHLEN);
        symmetric.mix_hash(re, DHLEN);
        offset = DHLEN;

        // Payload (plaintext — без ключа)
        size_t plen;
        if (!symmetric.decrypt_and_hash(msg + offset, msg_len - offset,
                                         payload_out, &plen))
            return false;
        *payload_len = plen;
        break;
    }

    case 1: {
        // ══ msg2: ← e, ee, s, es ════════════════════════════════════════
        if (msg_len < DHLEN) return false;

        // e: read remote ephemeral
        std::memcpy(re, msg, DHLEN);
        symmetric.mix_hash(re, DHLEN);
        offset = DHLEN;

        // ee: DH(e_sk, re)
        if (!dh(dh_out, e_sk, re)) return false;
        symmetric.mix_key(dh_out, DHLEN);

        // s: read encrypted remote static (DHLEN + MACLEN bytes)
        const size_t s_enc_len = DHLEN + MACLEN;
        if (msg_len < offset + s_enc_len) return false;

        size_t dec_len;
        if (!symmetric.decrypt_and_hash(msg + offset, s_enc_len, rs, &dec_len))
            return false;
        offset += s_enc_len;

        // es: DH(initiator_e, responder_s)
        //   initiator читает msg2 → DH(e_sk, rs)
        //   responder читает msg2 → невозможно в XX
        if (initiator)
            { if (!dh(dh_out, e_sk, rs)) return false; }
        else
            { if (!dh(dh_out, s_sk, re)) return false; }
        symmetric.mix_key(dh_out, DHLEN);

        // Payload
        size_t plen;
        if (!symmetric.decrypt_and_hash(msg + offset, msg_len - offset,
                                         payload_out, &plen))
            return false;
        *payload_len = plen;
        break;
    }

    case 2: {
        // ══ msg3: → s, se ════════════════════════════════════════════════

        // s: read encrypted remote static (DHLEN + MACLEN bytes)
        const size_t s_enc_len = DHLEN + MACLEN;
        if (msg_len < s_enc_len) return false;

        size_t dec_len;
        if (!symmetric.decrypt_and_hash(msg, s_enc_len, rs, &dec_len))
            return false;
        offset = s_enc_len;

        // se: DH(initiator_s, responder_e)
        //   responder читает msg3 → DH(e_sk, rs)
        //   initiator читает msg3 → невозможно в XX
        if (initiator)
            { if (!dh(dh_out, s_sk, re)) return false; }
        else
            { if (!dh(dh_out, e_sk, rs)) return false; }
        symmetric.mix_key(dh_out, DHLEN);

        // Payload
        size_t plen;
        if (!symmetric.decrypt_and_hash(msg + offset, msg_len - offset,
                                         payload_out, &plen))
            return false;
        *payload_len = plen;
        break;
    }

    default:
        return false;
    }

    sodium_memzero(dh_out, sizeof(dh_out));
    ++step;
    return true;
}

void HandshakeState::split(CipherState& send, CipherState& recv) {
    CipherState c1, c2;
    symmetric.split(c1, c2);

    // c1 = initiator→responder, c2 = responder→initiator
    if (initiator) {
        send = c1;
        recv = c2;
    } else {
        send = c2;
        recv = c1;
    }
}

void HandshakeState::get_handshake_hash(uint8_t out[HASHLEN]) const {
    std::memcpy(out, symmetric.h, HASHLEN);
}

void HandshakeState::clear() {
    symmetric.cipher.clear();
    sodium_memzero(symmetric.ck, sizeof(symmetric.ck));
    sodium_memzero(symmetric.h,  sizeof(symmetric.h));
    sodium_memzero(s_pk, sizeof(s_pk));
    sodium_memzero(s_sk, sizeof(s_sk));
    sodium_memzero(e_pk, sizeof(e_pk));
    sodium_memzero(e_sk, sizeof(e_sk));
    sodium_memzero(rs,   sizeof(rs));
    sodium_memzero(re,   sizeof(re));
}

} // namespace gn::noise
