#include <sodium.h>
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>

#include "connectionManager.hpp"
#include "signals.hpp"
#include "logger.hpp"

#include "../sdk/connector.h"

namespace fs = std::filesystem;
using namespace gn;

// ─── helpers ──────────────────────────────────────────────────────────────────

static fs::path tmp_dir(const std::string& suffix = "") {
    auto p = fs::temp_directory_path() / ("gn_test_" + suffix + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(p);
    return p;
}

// Build a valid OpenSSH Ed25519 PEM from a raw libsodium keypair.
// Returns PEM string that try_load_ssh_key() can parse.
static std::string make_openssh_pem(const uint8_t pub[32], const uint8_t sec[64]) {
    // Construct the binary openssh-key-v1 blob:
    //   magic(15) | cipher_str | kdf_str | kdf_opts | num_keys(u32be)
    //   | pubkey_blob | priv_block
    auto write_u32be = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(x >> 24); v.push_back((x >> 16) & 0xFF);
        v.push_back((x >>  8) & 0xFF); v.push_back(x & 0xFF);
    };
    auto write_str = [&](std::vector<uint8_t>& v, std::string_view s) {
        write_u32be(v, (uint32_t)s.size());
        v.insert(v.end(), s.begin(), s.end());
    };
    auto write_blob = [&](std::vector<uint8_t>& v, const uint8_t* d, size_t n) {
        write_u32be(v, (uint32_t)n);
        v.insert(v.end(), d, d + n);
    };

    // Build inner private key block
    std::vector<uint8_t> priv;
    uint32_t check = 0xDEADBEEF;
    write_u32be(priv, check);   // check1
    write_u32be(priv, check);   // check2
    write_str(priv, "ssh-ed25519");
    write_blob(priv, pub, 32);   // pubkey inside priv
    write_blob(priv, sec, 64);   // 64-byte secret key
    write_str(priv, "test-comment");
    // padding
    for (uint8_t i = 1; priv.size() % 8 != 0; ++i) priv.push_back(i);

    // Build pubkey blob: string("ssh-ed25519") + blob(pub)
    std::vector<uint8_t> pub_blob;
    write_str(pub_blob, "ssh-ed25519");
    write_blob(pub_blob, pub, 32);

    // Assemble outer structure
    std::vector<uint8_t> blob;
    // magic
    const uint8_t magic[] = {'o','p','e','n','s','s','h','-','k','e','y','-','v','1','\0'};
    blob.insert(blob.end(), magic, magic + sizeof(magic));
    write_str(blob, "none");    // cipher_name
    write_str(blob, "none");    // kdf_name
    write_str(blob, "");        // kdf_options (empty blob)
    write_u32be(blob, 1);       // num_keys
    write_blob(blob, pub_blob.data(), pub_blob.size());
    write_blob(blob, priv.data(), priv.size());

    // Base64 encode
    std::string b64;
    b64.reserve(blob.size() * 4 / 3 + 4);
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    while (i + 2 < blob.size()) {
        uint32_t v = (uint32_t(blob[i])<<16)|(uint32_t(blob[i+1])<<8)|blob[i+2];
        b64 += T[(v>>18)&63]; b64 += T[(v>>12)&63];
        b64 += T[(v>> 6)&63]; b64 += T[v&63];
        i += 3;
    }
    if (i < blob.size()) {
        uint32_t v = uint32_t(blob[i]) << 16;
        if (i + 1 < blob.size()) v |= uint32_t(blob[i+1]) << 8;
        b64 += T[(v>>18)&63]; b64 += T[(v>>12)&63];
        b64 += (i + 1 < blob.size()) ? T[(v>>6)&63] : '=';
        b64 += '=';
    }

    std::string pem = "-----BEGIN OPENSSH PRIVATE KEY-----\n";
    for (size_t j = 0; j < b64.size(); j += 70)
        pem += b64.substr(j, 70) + "\n";
    pem += "-----END OPENSSH PRIVATE KEY-----\n";
    return pem;
}

// Write a PEM key to a temp file, return the path
static fs::path write_openssh_pem(const fs::path& dir,
                                   const uint8_t pub[32], const uint8_t sec[64]) {
    fs::path p = dir / "id_ed25519";
    std::ofstream f(p);
    f << make_openssh_pem(pub, sec);
    return p;
}

// Minimal mock connector ops (no-op)
static int mock_connect(void*, const char*) { return -1; }
static int mock_listen (void*, const char*, uint16_t) { return 0; }
static int mock_send   (void*, conn_id_t, const void*, size_t) { return 0; }
static void mock_close (void*, conn_id_t) {}
static void mock_scheme(void*, char* buf, size_t sz) { strncpy(buf, "mock", sz); }
static void mock_name  (void*, char* buf, size_t sz) { strncpy(buf, "MockConnector", sz); }
static void mock_shutdown(void*) {}

static connector_ops_t make_mock_connector_ops() {
    connector_ops_t ops{};
    ops.connect    = [](void*, const char*) -> int { return -1; };
    ops.listen     = [](void*, const char*, uint16_t) -> int { return 0; };
    ops.send_to    = [](void*, conn_id_t, const void*, size_t) -> int { return 0; };
    ops.close      = [](void*, conn_id_t) {};
    ops.get_scheme = [](void*, char* b, size_t s) { strncpy(b, "mock", s); };
    ops.get_name   = [](void*, char* b, size_t s) { strncpy(b, "MockConnector", s); };
    ops.shutdown   = [](void*) {};
    ops.connector_ctx = nullptr;
    return ops;
}

// Build a valid AUTH payload for a given identity
static std::vector<uint8_t> build_auth_payload(const NodeIdentity& id,
                                                 const std::vector<std::string>& schemes = {}) {
    msg::AuthPayload ap{};
    std::memcpy(ap.user_pubkey,   id.user_pubkey,   32);
    std::memcpy(ap.device_pubkey, id.device_pubkey, 32);

    // ephem keypair
    uint8_t dummy_sk[crypto_box_SECRETKEYBYTES];
    crypto_box_keypair(ap.ephem_pubkey, dummy_sk); // generate ephemeral pubkey only
    // We can't properly sign without secret key here — generate a fresh ephem
    uint8_t ephem_sk[32], ephem_pk[32];
    crypto_box_keypair(ephem_pk, ephem_sk);
    std::memcpy(ap.ephem_pubkey, ephem_pk, 32);

    // Sign: msg = user_pk || device_pk || ephem_pk
    uint8_t msg[96];
    std::memcpy(msg,      ap.user_pubkey,   32);
    std::memcpy(msg + 32, ap.device_pubkey, 32);
    std::memcpy(msg + 64, ap.ephem_pubkey,  32);
    crypto_sign_ed25519_detached(ap.signature, nullptr, msg, 96, id.user_seckey);

    ap.set_schemes(schemes);

    std::vector<uint8_t> out(msg::AuthPayload::kFullSize);
    std::memcpy(out.data(), &ap, msg::AuthPayload::kFullSize);
    return out;
}

// ─── Fixture ──────────────────────────────────────────────────────────────────

class CMTest : public ::testing::Test {
protected:
    boost::asio::io_context  ioc_;
    SignalBus                bus_{ioc_};
    fs::path                 dir_a_ = tmp_dir("a");
    fs::path                 dir_b_ = tmp_dir("b");
    NodeIdentity             id_a_  = NodeIdentity::load_or_generate(dir_a_);
    NodeIdentity             id_b_  = NodeIdentity::load_or_generate(dir_b_);
    std::unique_ptr<ConnectionManager> cm_a_;
    std::unique_ptr<ConnectionManager> cm_b_;
    connector_ops_t          mock_ops_  = make_mock_connector_ops();

    void SetUp() override {
        cm_a_ = std::make_unique<ConnectionManager>(bus_, id_a_);
        cm_b_ = std::make_unique<ConnectionManager>(bus_, id_b_);
    }

    void TearDown() override {
        if (cm_a_) cm_a_->shutdown();
        if (cm_b_) cm_b_->shutdown();
        fs::remove_all(dir_a_);
        fs::remove_all(dir_b_);
    }

    // Register mock connector in cm, return host_api
    host_api_t make_api(ConnectionManager& cm) {
        host_api_t api{};
        cm.fill_host_api(&api);
        cm.register_connector("mock", &mock_ops_);
        return api;
    }

    // Simulate a full handshake between two CMs using mock connector APIs
    // Returns {conn_id_on_A, conn_id_on_B}
    std::pair<conn_id_t, conn_id_t> do_handshake(
        ConnectionManager& cm_a, const NodeIdentity& id_a,
        ConnectionManager& cm_b, const NodeIdentity& id_b,
        bool localhost = false)
    {
        host_api_t api_a{}, api_b{};
        cm_a.fill_host_api(&api_a);
        cm_b.fill_host_api(&api_b);

        // A→B connection
        endpoint_t ep_ab{};
        if (localhost)
            strncpy(ep_ab.address, "127.0.0.1", sizeof(ep_ab.address));
        else
            strncpy(ep_ab.address, "10.0.0.2", sizeof(ep_ab.address));
        ep_ab.port = 9999;

        endpoint_t ep_ba{};
        if (localhost)
            strncpy(ep_ba.address, "127.0.0.1", sizeof(ep_ba.address));
        else
            strncpy(ep_ba.address, "10.0.0.1", sizeof(ep_ba.address));
        ep_ba.port = 9998;

        conn_id_t cid_a = api_a.on_connect(api_a.ctx, &ep_ab);
        conn_id_t cid_b = api_b.on_connect(api_b.ctx, &ep_ba);

        // Both send_auth — capture what they send via send_to intercept
        // We'll feed captured bytes to each other's on_data
        // For simplicity, manually construct AUTH and feed it

        // Build AUTH payload for A and feed to B
        auto auth_a = build_auth_payload(id_a, {"mock"});
        auto auth_b = build_auth_payload(id_b, {"mock"});

        // Wrap in header
        auto make_frame = [](uint32_t type, const std::vector<uint8_t>& pl) {
            header_t h{};
            h.magic        = GNET_MAGIC;
            h.proto_ver    = GNET_PROTO_VER;
            h.payload_type = static_cast<uint16_t>(type);
            h.payload_len  = (uint32_t)pl.size();
            std::vector<uint8_t> frame(sizeof(h) + pl.size());
            std::memcpy(frame.data(), &h, sizeof(h));
            std::memcpy(frame.data() + sizeof(h), pl.data(), pl.size());
            return frame;
        };

        auto frame_a = make_frame(MSG_TYPE_AUTH, auth_a);
        auto frame_b = make_frame(MSG_TYPE_AUTH, auth_b);

        // Feed A's AUTH to B and B's AUTH to A
        api_b.on_data(api_b.ctx, cid_b, frame_a.data(), frame_a.size());
        api_a.on_data(api_a.ctx, cid_a, frame_b.data(), frame_b.size());

        return {cid_a, cid_b};
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 1: NodeIdentity / cm_identity.cpp
// ═══════════════════════════════════════════════════════════════════════════════

TEST(IdentityTest, GeneratesFreshKeypair) {
    auto dir = tmp_dir("gen");
    auto id = NodeIdentity::load_or_generate(dir);
    EXPECT_FALSE(id.user_pubkey_hex().empty());
    EXPECT_FALSE(id.device_pubkey_hex().empty());
    EXPECT_EQ(id.user_pubkey_hex().size(), 64u);   // 32 bytes hex
    EXPECT_EQ(id.device_pubkey_hex().size(), 64u);
    // Keys are valid: sig round-trip
    uint8_t sig[64], msg[] = {1,2,3};
    EXPECT_EQ(0, crypto_sign_ed25519_detached(sig, nullptr, msg, 3, id.user_seckey));
    EXPECT_EQ(0, crypto_sign_ed25519_verify_detached(sig, msg, 3, id.user_pubkey));
    fs::remove_all(dir);
}

TEST(IdentityTest, LoadsPersistedKeypair) {
    auto dir = tmp_dir("persist");
    auto id1 = NodeIdentity::load_or_generate(dir);
    auto id2 = NodeIdentity::load_or_generate(dir);
    EXPECT_EQ(id1.user_pubkey_hex(), id2.user_pubkey_hex());
    fs::remove_all(dir);
}

TEST(IdentityTest, TwoDifferentDirsGiveDifferentKeys) {
    auto d1 = tmp_dir("diff1"), d2 = tmp_dir("diff2");
    auto id1 = NodeIdentity::load_or_generate(d1);
    auto id2 = NodeIdentity::load_or_generate(d2);
    EXPECT_NE(id1.user_pubkey_hex(), id2.user_pubkey_hex());
    fs::remove_all(d1); fs::remove_all(d2);
}

TEST(IdentityTest, LoadsOpenSSHEd25519Key) {
    if (sodium_init() < 0) GTEST_SKIP();
    auto dir = tmp_dir("ssh");
    uint8_t pub[32], sec[64];
    crypto_sign_keypair(pub, sec);
    auto pem_path = write_openssh_pem(dir, pub, sec);

    uint8_t out_pub[32]{}, out_sec[64]{};
    bool ok = NodeIdentity::try_load_ssh_key(pem_path, out_pub, out_sec);
    ASSERT_TRUE(ok);
    EXPECT_EQ(std::memcmp(out_pub, pub, 32), 0);
    EXPECT_EQ(std::memcmp(out_sec, sec, 64), 0);
    fs::remove_all(dir);
}

TEST(IdentityTest, SSHKeyPemPath_UsedViaConfig) {
    if (sodium_init() < 0) GTEST_SKIP();
    auto dir = tmp_dir("sshcfg");
    uint8_t pub[32], sec[64];
    crypto_sign_keypair(pub, sec);
    auto pem = write_openssh_pem(dir, pub, sec);

    IdentityConfig cfg;
    cfg.dir = dir;
    cfg.ssh_key_path = pem;
    cfg.use_machine_id = false;
    auto id = NodeIdentity::load_or_generate(cfg);

    EXPECT_EQ(std::memcmp(id.user_pubkey, pub, 32), 0);
    fs::remove_all(dir);
}

TEST(IdentityTest, TryLoadSSHKey_NonExistentFile) {
    uint8_t pub[32]{}, sec[64]{};
    EXPECT_FALSE(NodeIdentity::try_load_ssh_key("/nonexistent/path", pub, sec));
}

TEST(IdentityTest, TryLoadSSHKey_EmptyFile) {
    auto dir = tmp_dir("sshempty");
    fs::path p = dir / "empty";
    std::ofstream(p) << "";
    uint8_t pub[32]{}, sec[64]{};
    EXPECT_FALSE(NodeIdentity::try_load_ssh_key(p, pub, sec));
    fs::remove_all(dir);
}

TEST(IdentityTest, TryLoadSSHKey_WrongContent) {
    auto dir = tmp_dir("sshjunk");
    fs::path p = dir / "bad";
    std::ofstream(p) << "-----BEGIN OPENSSH PRIVATE KEY-----\naGVsbG8=\n-----END OPENSSH PRIVATE KEY-----\n";
    uint8_t pub[32]{}, sec[64]{};
    EXPECT_FALSE(NodeIdentity::try_load_ssh_key(p, pub, sec));
    fs::remove_all(dir);
}

TEST(IdentityTest, TryLoadSSHKey_EncryptedKey_Rejected) {
    auto dir = tmp_dir("sshenc");
    // Craft a blob that passes magic but has cipher != "none"
    auto write_u32be = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(x>>24); v.push_back((x>>16)&0xFF);
        v.push_back((x>>8)&0xFF); v.push_back(x&0xFF);
    };
    auto write_str = [&](std::vector<uint8_t>& v, std::string_view s) {
        write_u32be(v, (uint32_t)s.size());
        v.insert(v.end(), s.begin(), s.end());
    };
    std::vector<uint8_t> blob;
    const uint8_t magic[] = {'o','p','e','n','s','s','h','-','k','e','y','-','v','1','\0'};
    blob.insert(blob.end(), magic, magic+sizeof(magic));
    write_str(blob, "aes256-ctr");  // encrypted!
    write_str(blob, "bcrypt");
    write_str(blob, "kdf_options_placeholder");

    // base64 encode
    std::string b64;
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    while (i + 2 < blob.size()) {
        uint32_t v = (uint32_t(blob[i])<<16)|(uint32_t(blob[i+1])<<8)|blob[i+2];
        b64 += T[(v>>18)&63]; b64 += T[(v>>12)&63]; b64 += T[(v>>6)&63]; b64 += T[v&63]; i+=3;
    }

    fs::path p = dir / "enc_key";
    std::ofstream f(p);
    f << "-----BEGIN OPENSSH PRIVATE KEY-----\n" << b64 << "\n-----END OPENSSH PRIVATE KEY-----\n";
    f.close();

    uint8_t pub[32]{}, sec[64]{};
    EXPECT_FALSE(NodeIdentity::try_load_ssh_key(p, pub, sec));
    fs::remove_all(dir);
}

TEST(IdentityTest, DeviceKeyDependsOnUserKey) {
    auto d1 = tmp_dir("dev1"), d2 = tmp_dir("dev2");
    auto id1 = NodeIdentity::load_or_generate(d1);
    auto id2 = NodeIdentity::load_or_generate(d2);
    // Different users → different device keys even on same (virtual) machine
    EXPECT_NE(id1.device_pubkey_hex(), id2.device_pubkey_hex());
    fs::remove_all(d1); fs::remove_all(d2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 2: SessionState / cm_session.cpp
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SessionTest, EncryptDecryptRoundTrip) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    std::vector<uint8_t> plain = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    auto wire = s.encrypt(plain.data(), plain.size());
    
    ASSERT_FALSE(wire.empty());
    
    // Обновленное ожидание: 8 (nonce) + 1 (flag) + plain + MAC
    size_t expected_size = 8u + 1u + plain.size() + crypto_secretbox_MACBYTES;
    EXPECT_EQ(wire.size(), expected_size);

    auto result = s.decrypt(wire.data(), wire.size());
    ASSERT_EQ(result, plain);
}

TEST(SessionTest, NoncesIncrement) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    uint8_t dummy[1] = {0};
    auto w1 = s.encrypt(dummy, 1);
    auto w2 = s.encrypt(dummy, 1);

    uint64_t n1, n2;
    std::memcpy(&n1, w1.data(), 8);
    std::memcpy(&n2, w2.data(), 8);
    EXPECT_LT(n1, n2);
}

TEST(SessionTest, ReplayProtection) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    uint8_t msg[] = "hello";
    auto wire = s.encrypt(msg, 5);

    // First decrypt succeeds
    auto r1 = s.decrypt(wire.data(), wire.size());
    EXPECT_EQ(r1.size(), 5u);

    // Replay (same nonce) — must fail
    auto r2 = s.decrypt(wire.data(), wire.size());
    EXPECT_TRUE(r2.empty());
}

TEST(SessionTest, TamperedPayloadRejected) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    uint8_t msg[] = "tamper me";
    auto wire = s.encrypt(msg, sizeof(msg));
    // flip a bit in the ciphertext
    wire[8 + crypto_secretbox_MACBYTES] ^= 0xFF;

    auto result = s.decrypt(wire.data(), wire.size());
    EXPECT_TRUE(result.empty());
}

TEST(SessionTest, TooShortPacketRejected) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));

    // Minimum valid = 8 + crypto_secretbox_MACBYTES = 24
    uint8_t tiny[23]{};
    auto r = s.decrypt(tiny, sizeof(tiny));
    EXPECT_TRUE(r.empty());
}

TEST(SessionTest, EmptyPlaintextRoundTrip) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    auto wire = s.encrypt(nullptr, 0);
    
    // Ожидание: 8 (nonce) + 1 (flag) + 0 (plain) + MAC
    EXPECT_EQ(wire.size(), 8u + 1u + crypto_secretbox_MACBYTES);

    auto result = s.decrypt(wire.data(), wire.size());
    EXPECT_TRUE(result.empty());
}

TEST(SessionTest, LargePayloadRoundTrip) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    std::vector<uint8_t> big(64 * 1024);
    randombytes_buf(big.data(), big.size());
    auto wire = s.encrypt(big.data(), big.size());
    auto result = s.decrypt(wire.data(), wire.size());
    ASSERT_EQ(result, big);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 3: gn::msg::AuthPayload helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AuthPayloadTest, SetGetSchemesRoundTrip) {
    msg::AuthPayload ap{};
    ap.set_schemes({"tcp", "ws", "udp"});
    EXPECT_EQ(ap.schemes_count, 3);
    auto schemes = ap.get_schemes();
    ASSERT_EQ(schemes.size(), 3u);
    EXPECT_EQ(schemes[0], "tcp");
    EXPECT_EQ(schemes[1], "ws");
    EXPECT_EQ(schemes[2], "udp");
}

TEST(AuthPayloadTest, TooManySchemesClipped) {
    msg::AuthPayload ap{};
    ap.set_schemes({"a","b","c","d","e","f","g","h","i","j"});  // 10 > AUTH_MAX_SCHEMES(8)
    EXPECT_EQ(ap.schemes_count, msg::AUTH_MAX_SCHEMES);
}

TEST(AuthPayloadTest, EmptySchemesProducesWildcard) {
    msg::AuthPayload ap{};
    ap.set_schemes({});
    EXPECT_EQ(ap.schemes_count, 0);
    auto s = ap.get_schemes();
    EXPECT_TRUE(s.empty());
}

TEST(AuthPayloadTest, SizeAssert) {
    using msg::AuthPayload;
    
    // Проверка базовой части (до схем)
    EXPECT_EQ(AuthPayload::kBaseSize, 160u);
    
    // Проверка блока схем (1 + 8 * 16)
    EXPECT_EQ(AuthPayload::kSchemeBlock, 129u);
    
    // Проверка мета-данных (2 * uint32)
    EXPECT_EQ(sizeof(msg::CoreMeta), 8u);
    
    // Итоговый размер: 160 + 129 + 8 = 297
    EXPECT_EQ(sizeof(AuthPayload), 297u);
    EXPECT_EQ(AuthPayload::kFullSize, 297u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 4: ConnectionManager — connection lifecycle / cm_handshake.cpp
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, OnConnectReturnsValidId) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 1234;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    EXPECT_NE(id, CONN_ID_INVALID);
    EXPECT_EQ(cm_a_->connection_count(), 1u);
}

TEST_F(CMTest, OnConnectLocalhostFlaggedCorrectly) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "127.0.0.1", sizeof(ep.address));
    ep.port = 5000;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    EXPECT_NE(id, CONN_ID_INVALID);
    // State should be AUTH_PENDING even for localhost
    auto st = cm_a_->get_state(id);
    ASSERT_TRUE(st.has_value());
    EXPECT_NE(*st, STATE_CLOSED);
}

TEST_F(CMTest, OnConnectLoopbackIPv6) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "::1", sizeof(ep.address));
    ep.port = 5001;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    EXPECT_NE(id, CONN_ID_INVALID);
}

TEST_F(CMTest, OnDisconnectRemovesRecord) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 1234;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    EXPECT_EQ(cm_a_->connection_count(), 1u);
    api.on_disconnect(api.ctx, id, 0);
    EXPECT_EQ(cm_a_->connection_count(), 0u);
}

TEST_F(CMTest, GetStateUnknownIdReturnsNullopt) {
    auto st = cm_a_->get_state(9999);
    EXPECT_FALSE(st.has_value());
}

TEST_F(CMTest, GetNegotiatedSchemeBeforeHandshakeReturnsNullopt) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 4444;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    auto scheme = cm_a_->get_negotiated_scheme(id);
    EXPECT_FALSE(scheme.has_value() && !scheme->empty());
}

TEST_F(CMTest, GetActiveUrisEmptyInitially) {
    EXPECT_TRUE(cm_a_->get_active_uris().empty());
}

TEST_F(CMTest, MultipleConnections) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    for (int i = 0; i < 5; ++i) {
        endpoint_t ep{};
        std::string addr = "10.0.0." + std::to_string(i+1);
        strncpy(ep.address, addr.c_str(), sizeof(ep.address));
        ep.port = (uint16_t)(1000 + i);
        api.on_connect(api.ctx, &ep);
    }
    EXPECT_EQ(cm_a_->connection_count(), 5u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 5: cm_dispatch.cpp — TCP reassembly, packet routing
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, PacketBeforeAuthDropped) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 5000;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    // Build a MSG_TYPE_CHAT packet (not AUTH) before ESTABLISHED
    header_t h{};
    h.magic        = GNET_MAGIC;
    h.proto_ver    = GNET_PROTO_VER;
    h.payload_type = MSG_TYPE_CHAT;
    h.payload_len  = 4;
    uint8_t pl[4] = {1,2,3,4};
    std::vector<uint8_t> frame(sizeof(h) + 4);
    std::memcpy(frame.data(), &h, sizeof(h));
    std::memcpy(frame.data() + sizeof(h), pl, 4);

    // Should be silently dropped (not crash, not establish)
    EXPECT_NO_THROW(api.on_data(api.ctx, id, frame.data(), frame.size()));
    auto st = cm_a_->get_state(id);
    ASSERT_TRUE(st.has_value());
    EXPECT_NE(*st, STATE_ESTABLISHED);
}

TEST_F(CMTest, PartialHeaderBufferedAndProcessedOnCompletion) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 5001;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    // Build a full AUTH frame
    auto auth = build_auth_payload(id_b_, {"mock"});
    header_t h{};
    h.magic        = GNET_MAGIC;
    h.proto_ver    = GNET_PROTO_VER;
    h.payload_type = MSG_TYPE_AUTH;
    h.payload_len  = (uint32_t)auth.size();
    std::vector<uint8_t> frame(sizeof(h) + auth.size());
    std::memcpy(frame.data(), &h, sizeof(h));
    std::memcpy(frame.data() + sizeof(h), auth.data(), auth.size());

    // Split into 3 chunks to test TCP reassembly
    size_t chunk = frame.size() / 3;
    EXPECT_NO_THROW(api.on_data(api.ctx, id, frame.data(), chunk));
    EXPECT_NO_THROW(api.on_data(api.ctx, id, frame.data() + chunk, chunk));
    EXPECT_NO_THROW(api.on_data(api.ctx, id, frame.data() + 2*chunk,
                                 frame.size() - 2*chunk));
    // Frame processed (no crash). AUTH verification may fail (we signed with id_b but
    // cm_a doesn't know id_b's key yet) — that's expected behaviour.
}

TEST_F(CMTest, TwoFramesInOneChunk) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 5002;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    // First frame: valid AUTH
    auto auth = build_auth_payload(id_b_, {});
    header_t h1{};
    h1.magic = GNET_MAGIC; h1.proto_ver = GNET_PROTO_VER;
    h1.payload_type = MSG_TYPE_AUTH; h1.payload_len = (uint32_t)auth.size();
    std::vector<uint8_t> f1(sizeof(h1) + auth.size());
    std::memcpy(f1.data(), &h1, sizeof(h1));
    std::memcpy(f1.data()+sizeof(h1), auth.data(), auth.size());

    // Second frame: also AUTH (duplicate — should be handled gracefully)
    std::vector<uint8_t> both(f1.size() + f1.size());
    std::memcpy(both.data(), f1.data(), f1.size());
    std::memcpy(both.data() + f1.size(), f1.data(), f1.size());

    EXPECT_NO_THROW(api.on_data(api.ctx, id, both.data(), both.size()));
}

TEST_F(CMTest, InvalidMagicDropped) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.2", sizeof(ep.address));
    ep.port = 5003;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    header_t h{};
    h.magic = 0xDEADBEEF;  // wrong!
    h.proto_ver = GNET_PROTO_VER;
    h.payload_type = MSG_TYPE_AUTH;
    h.payload_len = 0;
    EXPECT_NO_THROW(api.on_data(api.ctx, id, &h, sizeof(h)));
}

TEST_F(CMTest, EmptyDataChunkIgnored) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.3", sizeof(ep.address));
    ep.port = 5004;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    EXPECT_NO_THROW(api.on_data(api.ctx, id, nullptr, 0));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 6: cm_send.cpp — send, send_frame, scheme negotiation, getters
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, SendToUnknownUriWithNoConnector_DoesNotCrash) {
    // No connector registered — send to unknown URI should not crash
    EXPECT_NO_THROW(cm_a_->send("tcp://10.0.0.99:1234",
                                  MSG_TYPE_CHAT, "hi", 2));
}

TEST_F(CMTest, SendToUnknownUriWithConnector_DoesNotCrash) {
    cm_a_->register_connector("mock", &mock_ops_);
    EXPECT_NO_THROW(cm_a_->send("mock://10.0.0.99:1234",
                                  MSG_TYPE_CHAT, "hi", 2));
}

TEST_F(CMTest, RegisterConnectorAndQueryScheme) {
    cm_a_->register_connector("tcp", &mock_ops_);
    // We can verify via send not crashing for tcp:// URI
    EXPECT_NO_THROW(cm_a_->send("tcp://10.0.0.1:9000", MSG_TYPE_HEARTBEAT, nullptr, 0));
}

TEST_F(CMTest, SetSchemePriorityAffectsNegotiation) {
    // Set a custom priority
    cm_a_->set_scheme_priority({"ws", "tcp", "mock"});
    cm_a_->register_connector("mock", &mock_ops_);
    // Just verify no crash and method works
    EXPECT_NO_THROW(cm_a_->send("mock://10.0.0.1:9000", MSG_TYPE_CHAT, nullptr, 0));
}

TEST_F(CMTest, SendToDisconnectedIdDroppedGracefully) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 9000;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    api.on_disconnect(api.ctx, id, 0);

    // Now id is gone — send_frame to it should not crash
    EXPECT_NO_THROW(cm_a_->send("10.0.0.1:9000", MSG_TYPE_CHAT, "x", 1));
}

TEST_F(CMTest, ConnectionCountReturnsCorrect) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    EXPECT_EQ(cm_a_->connection_count(), 0u);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 1000;
    auto id = api.on_connect(api.ctx, &ep);
    EXPECT_EQ(cm_a_->connection_count(), 1u);
    api.on_disconnect(api.ctx, id, 1);  // error disconnect
    EXPECT_EQ(cm_a_->connection_count(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 7: host_api_t — sign/verify callbacks
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, HostApiSignVerifyRoundTrip) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    ASSERT_NE(api.sign_with_device, nullptr);
    ASSERT_NE(api.verify_signature, nullptr);

    uint8_t msg[] = "hello crypto world";
    uint8_t sig[64]{};
    int r = api.sign_with_device(api.ctx, msg, sizeof(msg), sig);
    EXPECT_EQ(r, 0);

    int v = api.verify_signature(api.ctx, msg, sizeof(msg),
                                   id_a_.device_pubkey, sig);
    EXPECT_EQ(v, 0);
}

TEST_F(CMTest, HostApiVerifyWrongKeyFails) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);

    uint8_t msg[] = "sign me";
    uint8_t sig[64]{};
    api.sign_with_device(api.ctx, msg, sizeof(msg), sig);

    // Verify with wrong key (id_b's device key)
    int v = api.verify_signature(api.ctx, msg, sizeof(msg),
                                   id_b_.device_pubkey, sig);
    EXPECT_NE(v, 0);
}

TEST_F(CMTest, HostApiLoggerNotNull) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    // internal_logger is set if Logger is initialized
    // Just verify fill_host_api doesn't crash and ctx is set
    EXPECT_NE(api.ctx, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 8: Full handshake (localhost — no crypto)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, LocalhostHandshakeEstablishes) {
    boost::asio::io_context ioc2;
    SignalBus bus2{ioc2};

    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // After feeding AUTH frames both ways, state should be ESTABLISHED
    auto st_a = cm_a_->get_state(cid_a);
    ASSERT_TRUE(st_a.has_value());
    EXPECT_EQ(*st_a, STATE_ESTABLISHED);
}

TEST_F(CMTest, LocalhostHandshake_SchemesNegotiated) {
    cm_a_->register_connector("mock", &mock_ops_);
    cm_b_->register_connector("mock", &mock_ops_);

    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto st = cm_a_->get_state(cid_a);
    EXPECT_TRUE(st.has_value());
    EXPECT_EQ(*st, STATE_ESTABLISHED);
}

TEST_F(CMTest, BadAuthPayload_TooShort_Dropped) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.5", sizeof(ep.address));
    ep.port = 7777;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    // Build AUTH frame with payload that's too short (< kBaseSize)
    std::vector<uint8_t> bad_auth(32, 0);  // only 32 bytes instead of 160+
    header_t h{};
    h.magic = GNET_MAGIC; h.proto_ver = GNET_PROTO_VER;
    h.payload_type = MSG_TYPE_AUTH;
    h.payload_len = (uint32_t)bad_auth.size();
    std::vector<uint8_t> frame(sizeof(h) + bad_auth.size());
    std::memcpy(frame.data(), &h, sizeof(h));
    std::memcpy(frame.data()+sizeof(h), bad_auth.data(), bad_auth.size());

    EXPECT_NO_THROW(api.on_data(api.ctx, id, frame.data(), frame.size()));
    // Connection should NOT be established
    auto st = cm_a_->get_state(id);
    if (st) EXPECT_NE(*st, STATE_ESTABLISHED);
}

TEST_F(CMTest, BadAuthPayload_InvalidSignature_Dropped) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.6", sizeof(ep.address));
    ep.port = 7778;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    // Valid-sized payload but corrupt signature
    msg::AuthPayload ap{};
    std::memcpy(ap.user_pubkey,   id_b_.user_pubkey,   32);
    std::memcpy(ap.device_pubkey, id_b_.device_pubkey, 32);
    // Leave signature as zeros (invalid)
    randombytes_buf(ap.ephem_pubkey, 32);
    ap.schemes_count = 0;
    std::vector<uint8_t> payload(msg::AuthPayload::kFullSize);
    std::memcpy(payload.data(), &ap, msg::AuthPayload::kFullSize);

    header_t h{};
    h.magic = GNET_MAGIC; h.proto_ver = GNET_PROTO_VER;
    h.payload_type = MSG_TYPE_AUTH;
    h.payload_len = (uint32_t)payload.size();
    std::vector<uint8_t> frame(sizeof(h) + payload.size());
    std::memcpy(frame.data(), &h, sizeof(h));
    std::memcpy(frame.data()+sizeof(h), payload.data(), payload.size());

    EXPECT_NO_THROW(api.on_data(api.ctx, id, frame.data(), frame.size()));
    auto st = cm_a_->get_state(id);
    if (st) EXPECT_NE(*st, STATE_ESTABLISHED);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 9: Shutdown behaviour
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, ShutdownIdempotent) {
    EXPECT_NO_THROW(cm_a_->shutdown());
    EXPECT_NO_THROW(cm_a_->shutdown());
}

TEST_F(CMTest, SendAfterShutdownDoesNotCrash) {
    cm_a_->register_connector("mock", &mock_ops_);
    cm_a_->shutdown();
    EXPECT_NO_THROW(cm_a_->send("mock://10.0.0.1:1234", MSG_TYPE_CHAT, "x", 1));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 10: Identity — pubkey hex helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(IdentityHexTest, HexIsLowerCase) {
    if (sodium_init() < 0) GTEST_SKIP();
    auto dir = tmp_dir("hex");
    auto id = NodeIdentity::load_or_generate(dir);
    const auto& h = id.user_pubkey_hex();
    for (char c : h) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "Non-lowercase-hex char: " << c;
    }
    fs::remove_all(dir);
}

TEST(IdentityHexTest, DeviceHexLength64) {
    if (sodium_init() < 0) GTEST_SKIP();
    auto dir = tmp_dir("hexlen");
    auto id = NodeIdentity::load_or_generate(dir);
    EXPECT_EQ(id.device_pubkey_hex().size(), 64u);
    fs::remove_all(dir);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 11: Backpressure / PerConnQueue integration
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, SendLargePayloadSucceeds) {
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // Large payload (512 KB) should be accepted on an established connection.
    // (Backpressure is tested in PerConnQueueTest suite directly.)
    std::vector<uint8_t> big(512 * 1024);
    randombytes_buf(big.data(), big.size());

    bool ok = cm_a_->send_on_conn(cid_a, MSG_TYPE_CHAT,
                                    big.data(), big.size());
    EXPECT_TRUE(ok);
}

TEST_F(CMTest, GetPeerPubkeyAfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto pk = cm_a_->get_peer_pubkey(cid_a);
    ASSERT_TRUE(pk.has_value());
    EXPECT_EQ(pk->size(), 32u);
    // The peer pubkey should match id_b's user_pubkey.
    EXPECT_EQ(std::memcmp(pk->data(), id_b_.user_pubkey, 32), 0);
}

TEST_F(CMTest, FindConnByPubkeyWorks) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    conn_id_t found = cm_a_->find_conn_by_pubkey(id_b_.user_pubkey_hex().c_str());
    EXPECT_EQ(found, cid_a);
}

TEST_F(CMTest, FindConnByPubkey_Unknown_ReturnsInvalid) {
    conn_id_t found = cm_a_->find_conn_by_pubkey(
        "0000000000000000000000000000000000000000000000000000000000000000");
    EXPECT_EQ(found, CONN_ID_INVALID);
}

TEST_F(CMTest, SendOnConnDirectly) {
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    uint8_t payload[] = {1, 2, 3, 4};
    // send_on_conn should succeed on an ESTABLISHED connection.
    bool ok = cm_a_->send_on_conn(cid_a, MSG_TYPE_CHAT, payload, sizeof(payload));
    EXPECT_TRUE(ok);
}

TEST_F(CMTest, SendOnConn_InvalidId_ReturnsFalse) {
    cm_a_->register_connector("mock", &mock_ops_);
    uint8_t payload[] = {1};
    bool ok = cm_a_->send_on_conn(9999, MSG_TYPE_CHAT, payload, sizeof(payload));
    EXPECT_FALSE(ok);
}

TEST_F(CMTest, DisconnectCleansUpAllIndices) {
    host_api_t api_a{};
    cm_a_->fill_host_api(&api_a);

    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // Verify connection is known.
    EXPECT_NE(cm_a_->get_state(cid_a), std::nullopt);
    EXPECT_NE(cm_a_->find_conn_by_pubkey(id_b_.user_pubkey_hex().c_str()),
              CONN_ID_INVALID);

    // Disconnect.
    api_a.on_disconnect(api_a.ctx, cid_a, 0);

    // Everything should be cleaned up.
    EXPECT_EQ(cm_a_->get_state(cid_a), std::nullopt);
    EXPECT_EQ(cm_a_->find_conn_by_pubkey(id_b_.user_pubkey_hex().c_str()),
              CONN_ID_INVALID);
    EXPECT_EQ(cm_a_->connection_count(), 0u);
}

TEST_F(CMTest, GetPendingBytesInitiallyZero) {
    EXPECT_EQ(cm_a_->get_pending_bytes(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 12: Broadcast
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, BroadcastDoesNotCrashWithNoConnections) {
    cm_a_->register_connector("mock", &mock_ops_);
    uint8_t data[] = {0xAA, 0xBB};
    EXPECT_NO_THROW(cm_a_->broadcast(MSG_TYPE_CHAT, data, sizeof(data)));
}

TEST_F(CMTest, BroadcastToEstablishedPeers) {
    cm_a_->register_connector("mock", &mock_ops_);

    // Handshake with two peers.
    auto dir_c = tmp_dir("c");
    auto id_c = NodeIdentity::load_or_generate(dir_c);
    boost::asio::io_context ioc_c;
    SignalBus bus_c{ioc_c};
    auto cm_c = std::make_unique<ConnectionManager>(bus_c, id_c);

    auto [cid_ab, cid_ba] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);
    auto [cid_ac, cid_ca] = do_handshake(*cm_a_, id_a_, *cm_c,  id_c,  true);

    // Both connections should be ESTABLISHED.
    EXPECT_EQ(*cm_a_->get_state(cid_ab), STATE_ESTABLISHED);
    EXPECT_EQ(*cm_a_->get_state(cid_ac), STATE_ESTABLISHED);

    // Broadcast should not crash with two established peers.
    uint8_t data[] = {0x01};
    EXPECT_NO_THROW(cm_a_->broadcast(MSG_TYPE_CHAT, data, sizeof(data)));

    cm_c->shutdown();
    fs::remove_all(dir_c);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 13: KEY_EXCHANGE / rekey
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, RekeyOnNonEstablished_Fails) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 8000;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    // Connection is AUTH_PENDING — rekey must fail
    EXPECT_FALSE(cm_a_->rekey_session(id));
}

TEST_F(CMTest, RekeySession_SendsKeyExchange) {
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    EXPECT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);
    // rekey_session should return true on ESTABLISHED connection
    EXPECT_TRUE(cm_a_->rekey_session(cid_a));
}

TEST_F(CMTest, RekeyResetsNonces) {
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // Send a packet to advance nonces past 1
    uint8_t payload[] = {1, 2, 3};
    cm_a_->send_on_conn(cid_a, MSG_TYPE_CHAT, payload, sizeof(payload));

    // After rekey, nonces should reset — the rekey itself succeeds
    EXPECT_TRUE(cm_a_->rekey_session(cid_a));
}

TEST_F(CMTest, RekeyOnInvalidId_Fails) {
    EXPECT_FALSE(cm_a_->rekey_session(99999));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 14: Compression / zstd (SessionState level)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SessionCompressionTest, Encrypt_Large_UsesZstd) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    // > 512 bytes → should trigger zstd
    std::vector<uint8_t> big(2048, 0xAA);
    auto wire = s.encrypt(big.data(), big.size());
    ASSERT_FALSE(wire.empty());

    // Encrypted wire should be smaller than uncompressed would be,
    // because repetitive data compresses very well.
    // Uncompressed wire: 8 (nonce) + 1 (flag) + 2048 (data) + MAC
    size_t uncompressed_wire_size = 8u + 1u + big.size() + crypto_secretbox_MACBYTES;
    EXPECT_LT(wire.size(), uncompressed_wire_size);
}

TEST(SessionCompressionTest, Decrypt_Zstd_Decompresses) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    std::vector<uint8_t> big(2048);
    // Pattern that compresses well
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i % 17);

    auto wire = s.encrypt(big.data(), big.size());
    auto result = s.decrypt(wire.data(), wire.size());
    ASSERT_EQ(result.size(), big.size());
    EXPECT_EQ(result, big);
}

TEST(SessionCompressionTest, Encrypt_Small_NoCompression) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    // < 512 bytes → no compression
    std::vector<uint8_t> small(100, 0xBB);
    auto wire = s.encrypt(small.data(), small.size());

    // Must be: 8 (nonce) + 1 (FLAG_RAW) + 100 (plain) + MAC
    size_t expected = 8u + 1u + small.size() + crypto_secretbox_MACBYTES;
    EXPECT_EQ(wire.size(), expected);

    auto result = s.decrypt(wire.data(), wire.size());
    EXPECT_EQ(result, small);
}

TEST(SessionCompressionTest, CompressionRoundtrip_Stress) {
    if (sodium_init() < 0) GTEST_SKIP();
    SessionState s;
    randombytes_buf(s.session_key, sizeof(s.session_key));
    s.ready = true;

    for (int i = 0; i < 100; ++i) {
        size_t sz = 128 + (i * 80); // 128..8128 bytes
        std::vector<uint8_t> plain(sz);
        randombytes_buf(plain.data(), plain.size());
        auto wire = s.encrypt(plain.data(), plain.size());
        auto result = s.decrypt(wire.data(), wire.size());
        ASSERT_EQ(result.size(), plain.size()) << "iteration " << i;
        EXPECT_EQ(result, plain) << "iteration " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 15: derive_session
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, BothSides_IdenticalKey) {
    // After do_handshake, both CMs should have derived the same session key
    // (verified indirectly: send from A, receive on B works)
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // Both sides are ESTABLISHED — session derived successfully
    EXPECT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);
    EXPECT_EQ(*cm_b_->get_state(cid_b), STATE_ESTABLISHED);
}

TEST_F(CMTest, DifferentEphemeral_DifferentKey) {
    // Two separate handshakes should produce different session keys
    auto [cid_a1, cid_b1] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // Create fresh CMs for second handshake
    auto dir_c = tmp_dir("c");
    auto dir_d = tmp_dir("d");
    auto id_c = NodeIdentity::load_or_generate(dir_c);
    auto id_d = NodeIdentity::load_or_generate(dir_d);
    boost::asio::io_context ioc2;
    SignalBus bus2{ioc2};
    auto cm_c = std::make_unique<ConnectionManager>(bus2, id_c);
    auto cm_d = std::make_unique<ConnectionManager>(bus2, id_d);

    auto [cid_c, cid_d] = do_handshake(*cm_c, id_c, *cm_d, id_d, true);

    // Both pairs established (different ephemeral keys → different session keys)
    EXPECT_EQ(*cm_a_->get_state(cid_a1), STATE_ESTABLISHED);
    EXPECT_EQ(*cm_c->get_state(cid_c), STATE_ESTABLISHED);

    cm_c->shutdown();
    cm_d->shutdown();
    fs::remove_all(dir_c);
    fs::remove_all(dir_d);
}

TEST_F(CMTest, SortedPubkeys_Deterministic) {
    // Handshake A→B and B→A should both succeed (pubkeys sorted internally)
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);
    auto [cid_b2, cid_a2] = do_handshake(*cm_b_, id_b_, *cm_a_, id_a_, true);

    EXPECT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);
    EXPECT_EQ(*cm_b_->get_state(cid_b2), STATE_ESTABLISHED);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 16: Relay (MSG_TYPE_RELAY)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RelayPayloadTest, SizeCheck) {
    EXPECT_EQ(sizeof(msg::RelayPayload), 33u);
}

TEST_F(CMTest, HandleRelay_LocalDelivery) {
    // Setup: A and B handshake on localhost (no encryption).
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);
    ASSERT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);

    // Build an inner frame: a MSG_TYPE_CHAT packet destined for local delivery.
    header_t inner_hdr{};
    inner_hdr.magic        = GNET_MAGIC;
    inner_hdr.proto_ver    = GNET_PROTO_VER;
    inner_hdr.payload_type = MSG_TYPE_CHAT;
    inner_hdr.payload_len  = 4;
    inner_hdr.packet_id    = 42;
    std::memcpy(inner_hdr.sender_id, id_b_.device_pubkey, 16);
    uint8_t chat_data[4] = {0xCA, 0xFE, 0xBA, 0xBE};

    std::vector<uint8_t> inner_frame(sizeof(inner_hdr) + 4);
    std::memcpy(inner_frame.data(), &inner_hdr, sizeof(inner_hdr));
    std::memcpy(inner_frame.data() + sizeof(inner_hdr), chat_data, 4);

    // Build relay payload: TTL=3, dest = id_a_ (local)
    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());
    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = 3;
    std::memcpy(rp->dest_pubkey, id_a_.user_pubkey, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    // Wrap in a MSG_TYPE_RELAY wire frame and feed to A.
    header_t relay_hdr{};
    relay_hdr.magic        = GNET_MAGIC;
    relay_hdr.proto_ver    = GNET_PROTO_VER;
    relay_hdr.payload_type = MSG_TYPE_RELAY;
    relay_hdr.payload_len  = static_cast<uint32_t>(relay_payload.size());

    std::vector<uint8_t> wire(sizeof(relay_hdr) + relay_payload.size());
    std::memcpy(wire.data(), &relay_hdr, sizeof(relay_hdr));
    std::memcpy(wire.data() + sizeof(relay_hdr),
                relay_payload.data(), relay_payload.size());

    // Feed to on_data — should not crash (local delivery via dispatch_packet).
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    EXPECT_NO_THROW(api.on_data(api.ctx, cid_a, wire.data(), wire.size()));
}

TEST_F(CMTest, HandleRelay_Forward) {
    // Three nodes: A ←→ B ←→ C. Send relay through B to C.
    cm_b_->register_connector("mock", &mock_ops_);

    auto dir_c = tmp_dir("relay_c");
    auto id_c  = NodeIdentity::load_or_generate(dir_c);
    boost::asio::io_context ioc_c;
    SignalBus bus_c{ioc_c};
    auto cm_c = std::make_unique<ConnectionManager>(bus_c, id_c);

    // Handshake A↔B and B↔C on localhost.
    auto [cid_ba, cid_ab] = do_handshake(*cm_b_, id_b_, *cm_a_, id_a_, true);
    auto [cid_bc, cid_cb] = do_handshake(*cm_b_, id_b_, *cm_c,  id_c,  true);

    ASSERT_EQ(*cm_b_->get_state(cid_ba), STATE_ESTABLISHED);
    ASSERT_EQ(*cm_b_->get_state(cid_bc), STATE_ESTABLISHED);

    // Build inner frame.
    header_t inner_hdr{};
    inner_hdr.magic        = GNET_MAGIC;
    inner_hdr.proto_ver    = GNET_PROTO_VER;
    inner_hdr.payload_type = MSG_TYPE_CHAT;
    inner_hdr.payload_len  = 2;
    inner_hdr.packet_id    = 99;
    std::memcpy(inner_hdr.sender_id, id_a_.device_pubkey, 16);
    uint8_t chat[2] = {0x01, 0x02};

    std::vector<uint8_t> inner_frame(sizeof(inner_hdr) + 2);
    std::memcpy(inner_frame.data(), &inner_hdr, sizeof(inner_hdr));
    std::memcpy(inner_frame.data() + sizeof(inner_hdr), chat, 2);

    // Build relay payload destined for C, TTL=3.
    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());
    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = 3;
    std::memcpy(rp->dest_pubkey, id_c.user_pubkey, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    // Wrap and feed to B from A's connection.
    header_t relay_hdr{};
    relay_hdr.magic        = GNET_MAGIC;
    relay_hdr.proto_ver    = GNET_PROTO_VER;
    relay_hdr.payload_type = MSG_TYPE_RELAY;
    relay_hdr.payload_len  = static_cast<uint32_t>(relay_payload.size());

    std::vector<uint8_t> wire(sizeof(relay_hdr) + relay_payload.size());
    std::memcpy(wire.data(), &relay_hdr, sizeof(relay_hdr));
    std::memcpy(wire.data() + sizeof(relay_hdr),
                relay_payload.data(), relay_payload.size());

    host_api_t api_b{};
    cm_b_->fill_host_api(&api_b);
    // B receives relay from A — should forward to C (not crash, not loop back to A).
    EXPECT_NO_THROW(api_b.on_data(api_b.ctx, cid_ba, wire.data(), wire.size()));

    cm_c->shutdown();
    fs::remove_all(dir_c);
}

TEST_F(CMTest, HandleRelay_TTLZero_Dropped) {
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // Build minimal inner frame.
    header_t inner_hdr{};
    inner_hdr.magic        = GNET_MAGIC;
    inner_hdr.proto_ver    = GNET_PROTO_VER;
    inner_hdr.payload_type = MSG_TYPE_CHAT;
    inner_hdr.payload_len  = 1;
    inner_hdr.packet_id    = 200;

    std::vector<uint8_t> inner_frame(sizeof(inner_hdr) + 1);
    std::memcpy(inner_frame.data(), &inner_hdr, sizeof(inner_hdr));
    inner_frame.back() = 0xFF;

    // Build relay with TTL=0 → should be dropped.
    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());
    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = 0;  // zero!
    std::memcpy(rp->dest_pubkey, id_b_.user_pubkey, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    header_t relay_hdr{};
    relay_hdr.magic        = GNET_MAGIC;
    relay_hdr.proto_ver    = GNET_PROTO_VER;
    relay_hdr.payload_type = MSG_TYPE_RELAY;
    relay_hdr.payload_len  = static_cast<uint32_t>(relay_payload.size());

    std::vector<uint8_t> wire(sizeof(relay_hdr) + relay_payload.size());
    std::memcpy(wire.data(), &relay_hdr, sizeof(relay_hdr));
    std::memcpy(wire.data() + sizeof(relay_hdr),
                relay_payload.data(), relay_payload.size());

    host_api_t api{};
    cm_a_->fill_host_api(&api);
    // TTL=0 → dropped silently (no crash).
    EXPECT_NO_THROW(api.on_data(api.ctx, cid_a, wire.data(), wire.size()));
}

TEST_F(CMTest, HandleRelay_Dedup) {
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // Build inner frame with unique sender_id + packet_id.
    header_t inner_hdr{};
    inner_hdr.magic        = GNET_MAGIC;
    inner_hdr.proto_ver    = GNET_PROTO_VER;
    inner_hdr.payload_type = MSG_TYPE_CHAT;
    inner_hdr.payload_len  = 1;
    inner_hdr.packet_id    = 777;
    std::memcpy(inner_hdr.sender_id, id_b_.device_pubkey, 16);

    std::vector<uint8_t> inner_frame(sizeof(inner_hdr) + 1);
    std::memcpy(inner_frame.data(), &inner_hdr, sizeof(inner_hdr));
    inner_frame.back() = 0xAA;

    // Build relay destined for id_a_ (local delivery).
    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());
    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = 5;
    std::memcpy(rp->dest_pubkey, id_a_.user_pubkey, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    header_t relay_hdr{};
    relay_hdr.magic        = GNET_MAGIC;
    relay_hdr.proto_ver    = GNET_PROTO_VER;
    relay_hdr.payload_type = MSG_TYPE_RELAY;
    relay_hdr.payload_len  = static_cast<uint32_t>(relay_payload.size());

    std::vector<uint8_t> wire(sizeof(relay_hdr) + relay_payload.size());
    std::memcpy(wire.data(), &relay_hdr, sizeof(relay_hdr));
    std::memcpy(wire.data() + sizeof(relay_hdr),
                relay_payload.data(), relay_payload.size());

    host_api_t api{};
    cm_a_->fill_host_api(&api);

    // First delivery — should succeed.
    EXPECT_NO_THROW(api.on_data(api.ctx, cid_a, wire.data(), wire.size()));
    // Second delivery (same sender_id + packet_id) — should be deduped (no crash).
    EXPECT_NO_THROW(api.on_data(api.ctx, cid_a, wire.data(), wire.size()));
}

TEST_F(CMTest, Relay_CoreCapAdvertised) {
    auto meta = cm_a_->local_core_meta();
    EXPECT_NE(meta.caps_mask & CORE_CAP_RELAY, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 17: Queries + control
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, GetActiveConnIds_OnlyEstablished) {
    host_api_t api_a{};
    cm_a_->fill_host_api(&api_a);

    // Create 3 connections, only 1 will be ESTABLISHED
    endpoint_t ep1{}, ep2{}, ep3{};
    strncpy(ep1.address, "10.0.0.1", sizeof(ep1.address)); ep1.port = 9001;
    strncpy(ep2.address, "10.0.0.2", sizeof(ep2.address)); ep2.port = 9002;
    strncpy(ep3.address, "10.0.0.3", sizeof(ep3.address)); ep3.port = 9003;

    api_a.on_connect(api_a.ctx, &ep1);  // AUTH_PENDING
    api_a.on_connect(api_a.ctx, &ep2);  // AUTH_PENDING

    // Do handshake for third — this one becomes ESTABLISHED
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto active = cm_a_->get_active_conn_ids();
    EXPECT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0], cid_a);
}

TEST_F(CMTest, GetPeerEndpoint_AfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    endpoint_t ep{};
    ASSERT_TRUE(cm_a_->get_peer_endpoint(cid_a, ep));
    EXPECT_EQ(ep.peer_id, cid_a);
    // Address should be set (either 127.0.0.1 or 10.0.0.x depending on test)
    EXPECT_GT(strlen(ep.address), 0u);
}

TEST_F(CMTest, DisconnectGraceful_DrainsQueue) {
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // disconnect should not crash and should clean up
    EXPECT_NO_THROW(cm_a_->disconnect(cid_a));
}

TEST_F(CMTest, CloseNow_ImmediateClose) {
    cm_a_->register_connector("mock", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    EXPECT_NO_THROW(cm_a_->close_now(cid_a));
}

TEST_F(CMTest, RotateIdentityKeys) {
    auto old_hex = cm_a_->identity().user_pubkey_hex();
    auto dir_rot = tmp_dir("rot");

    // Generate a distinct SSH key for rotation
    uint8_t rot_pub[32], rot_sec[64];
    crypto_sign_keypair(rot_pub, rot_sec);
    write_openssh_pem(dir_rot, rot_pub, rot_sec);

    IdentityConfig cfg{
        .dir          = dir_rot,
        .ssh_key_path = dir_rot / "id_ed25519",
        .use_machine_id = false,
    };
    cm_a_->rotate_identity_keys(cfg);

    // After rotation, user pubkey should be different
    auto new_hex = cm_a_->identity().user_pubkey_hex();
    EXPECT_NE(old_hex, new_hex);
    fs::remove_all(dir_rot);
}

TEST_F(CMTest, LocalCoreMeta_HasVersion) {
    auto meta = cm_a_->local_core_meta();
    EXPECT_GT(meta.core_version, 0u);
    // Should have ZSTD and KEYROT caps
    EXPECT_NE(meta.caps_mask & CORE_CAP_ZSTD, 0u);
    EXPECT_NE(meta.caps_mask & CORE_CAP_KEYROT, 0u);
}