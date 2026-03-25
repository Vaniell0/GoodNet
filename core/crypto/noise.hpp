#pragma once
/// @file core/crypto/noise.hpp
/// Noise_XX_25519_ChaChaPoly_BLAKE2b — протокольный движок.
///
/// Реализация Noise Framework (noiseprotocol.org) на libsodium:
///   DH:   X25519  (crypto_scalarmult)
///   AEAD: ChaChaPoly-IETF (crypto_aead_chacha20poly1305_ietf)
///   Hash: BLAKE2b-256 (crypto_generichash_blake2b, HASHLEN=32)
///
/// Handshake pattern XX (обе стороны передают static keys):
///   → e                     (msg1: initiator → responder)
///   ← e, ee, s, es          (msg2: responder → initiator)
///   → s, se                 (msg3: initiator → responder)

#include <cstddef>
#include <cstdint>

namespace gn::noise {

// ── Размеры ──────────────────────────────────────────────────────────────────

static constexpr size_t DHLEN    = 32;  ///< X25519 key / shared secret
static constexpr size_t HASHLEN  = 32;  ///< BLAKE2b output
static constexpr size_t KEYLEN   = 32;  ///< ChaChaPoly-IETF key
static constexpr size_t MACLEN   = 16;  ///< ChaChaPoly-IETF tag (Poly1305)
static constexpr size_t NONCELEN = 12;  ///< ChaChaPoly-IETF nonce

/// Имя протокола для InitSymmetric (34 байта > HASHLEN → хешируется).
static constexpr const char* PROTOCOL_NAME =
    "Noise_XX_25519_ChaChaPoly_BLAKE2b";

// ── CipherState ──────────────────────────────────────────────────────────────

/// AEAD symmetric state: ключ + монотонный счётчик nonce.
struct CipherState {
    uint8_t  key[KEYLEN]{};
    uint64_t nonce   = 0;
    bool     has_key = false;

    /// AEAD encrypt. out_len = plain_len + MACLEN.
    /// Если ключ не установлен — passthrough (копирует plain в out).
    bool encrypt(const uint8_t* ad, size_t ad_len,
                 const uint8_t* plain, size_t plain_len,
                 uint8_t* out, size_t* out_len);

    /// AEAD decrypt. out_len = cipher_len - MACLEN.
    bool decrypt(const uint8_t* ad, size_t ad_len,
                 const uint8_t* cipher, size_t cipher_len,
                 uint8_t* out, size_t* out_len);

    /// Noise rekey: k = ENCRYPT(k, maxnonce, "", ZEROS(32))[0:32].
    void rekey();

    /// Безопасное обнуление ключевого материала.
    void clear();
};

// ── SymmetricState ───────────────────────────────────────────────────────────

/// Chaining state: ck (chaining key) + h (handshake hash) + CipherState.
struct SymmetricState {
    uint8_t     ck[HASHLEN]{};
    uint8_t     h[HASHLEN]{};
    CipherState cipher;

    /// Инициализация с именем протокола.
    void init(const char* protocol_name);

    /// h = HASH(h || data).
    void mix_hash(const uint8_t* data, size_t len);

    /// ck, k = HKDF(ck, ikm). Устанавливает ключ в CipherState.
    void mix_key(const uint8_t* ikm, size_t len);

    /// Шифрует plain (или passthrough если нет ключа) и подмешивает в h.
    bool encrypt_and_hash(const uint8_t* plain, size_t plain_len,
                          uint8_t* out, size_t* out_len);

    /// Дешифрует cipher и подмешивает в h.
    bool decrypt_and_hash(const uint8_t* cipher, size_t cipher_len,
                          uint8_t* out, size_t* out_len);

    /// Вывод двух CipherState для транспорта.
    /// c1 = initiator→responder, c2 = responder→initiator.
    void split(CipherState& c1, CipherState& c2);
};

// ── HandshakeState ───────────────────────────────────────────────────────────

/// Полное состояние Noise_XX handshake.
struct HandshakeState {
    SymmetricState symmetric;

    uint8_t s_pk[DHLEN]{};  ///< Локальный static X25519 pk
    uint8_t s_sk[DHLEN]{};  ///< Локальный static X25519 sk
    uint8_t e_pk[DHLEN]{};  ///< Локальный ephemeral pk
    uint8_t e_sk[DHLEN]{};  ///< Локальный ephemeral sk
    uint8_t rs[DHLEN]{};    ///< Remote static pk
    uint8_t re[DHLEN]{};    ///< Remote ephemeral pk

    bool initiator = false;
    int  step      = 0;     ///< 0→msg1, 1→msg2, 2→msg3, 3→done

    /// Инициализация. static_pk/sk — X25519 (Ed25519 конвертировать до вызова).
    void init(bool is_initiator,
              const uint8_t static_pk[DHLEN],
              const uint8_t static_sk[DHLEN]);

    /// Записать следующее handshake-сообщение.
    /// @param payload    Данные для вложения (user_pk, schemes, meta и т.д.)
    /// @param out        Буфер вывода (минимум DHLEN*2 + MACLEN*2 + payload_len + MACLEN)
    /// @param out_len    [out] Размер записанного сообщения
    bool write_message(const uint8_t* payload, size_t payload_len,
                       uint8_t* out, size_t* out_len);

    /// Прочитать входящее handshake-сообщение и извлечь payload.
    bool read_message(const uint8_t* msg, size_t msg_len,
                      uint8_t* payload_out, size_t* payload_len);

    [[nodiscard]] bool is_complete() const { return step >= 3; }

    /// Вывести transport CipherState'ы.
    /// Initiator: send=c1, recv=c2. Responder: send=c2, recv=c1.
    void split(CipherState& send, CipherState& recv);

    /// Получить handshake hash (для channel binding / session identity).
    void get_handshake_hash(uint8_t out[HASHLEN]) const;

    /// Безопасное обнуление всего ключевого материала.
    void clear();
};

// ── Вспомогательные криптофункции ────────────────────────────────────────────

/// X25519 DH. false при невалидном public key.
bool dh(uint8_t out[DHLEN], const uint8_t sk[DHLEN], const uint8_t pk[DHLEN]);

/// Генерация X25519 ephemeral keypair.
void generate_keypair(uint8_t pk[DHLEN], uint8_t sk[DHLEN]);

/// BLAKE2b-256 хеш (без ключа).
void hash(uint8_t out[HASHLEN], const uint8_t* data, size_t len);

/// BLAKE2b-256 keyed hash (MAC / HMAC-replacement для HKDF).
void hmac_hash(uint8_t out[HASHLEN],
               const uint8_t key[HASHLEN],
               const uint8_t* data, size_t data_len);

/// HKDF(ck, ikm) → (out1, out2). ikm может быть nullptr при ikm_len=0.
void hkdf2(const uint8_t ck[HASHLEN],
           const uint8_t* ikm, size_t ikm_len,
           uint8_t out1[HASHLEN], uint8_t out2[HASHLEN]);

} // namespace gn::noise
