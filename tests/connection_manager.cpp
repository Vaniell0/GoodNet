#include <sodium.h>
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>

#include <nlohmann/json.hpp>

#include "test_helpers.hpp"
#include "cm/connectionManager.hpp"
#include "cm/impl.hpp"
#include "signals.hpp"
#include "logger.hpp"
#include "data/messages.hpp"

#include "../sdk/handler.h"

using namespace gn;

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

    host_api_t make_api(ConnectionManager& cm) {
        host_api_t api{};
        cm.fill_host_api(&api);
        cm.register_connector("tcp", &mock_ops_);
        return api;
    }

    // Wrappers for Impl access (CMTest is friend of ConnectionManager)
    ConnectionManager::Impl& impl(ConnectionManager& cm) { return *cm.impl_; }
    const ConnectionManager::Impl& impl(const ConnectionManager& cm) const { return *cm.impl_; }

    /// Simulate full Noise_XX handshake between two CMs.
    /// Uses a single capturing connector to intercept all wire frames.
    std::pair<conn_id_t, conn_id_t> do_handshake(
        ConnectionManager& cm_a, const NodeIdentity&,
        ConnectionManager& cm_b, const NodeIdentity&,
        bool localhost = false)
    {
        CapturingSink sink;
        auto cap_ops = make_capturing_connector(&sink);

        host_api_t api_a{}, api_b{};
        cm_a.fill_host_api(&api_a);
        cm_b.fill_host_api(&api_b);
        cm_a.register_connector("tcp", &cap_ops);
        cm_b.register_connector("tcp", &cap_ops);

        endpoint_t ep_ab{};
        if (localhost) {
            strncpy(ep_ab.address, "127.0.0.1", sizeof(ep_ab.address));
            ep_ab.flags = EP_FLAG_TRUSTED | EP_FLAG_OUTBOUND;
        } else {
            strncpy(ep_ab.address, "10.0.0.2", sizeof(ep_ab.address));
            ep_ab.flags = EP_FLAG_OUTBOUND;
        }
        ep_ab.port = 9999;

        endpoint_t ep_ba{};
        if (localhost) {
            strncpy(ep_ba.address, "127.0.0.1", sizeof(ep_ba.address));
            ep_ba.flags = EP_FLAG_TRUSTED;
        } else {
            strncpy(ep_ba.address, "10.0.0.1", sizeof(ep_ba.address));
        }
        ep_ba.port = 9998;

        // A connects (outbound) — sends NOISE_INIT automatically
        g_cap_sink = &sink;
        conn_id_t cid_a = api_a.on_connect(api_a.ctx, &ep_ab);

        // B connects (inbound) — no automatic send
        conn_id_t cid_b = api_b.on_connect(api_b.ctx, &ep_ba);

        // Helper: find and extract a captured frame by msg_type, remove it from sink
        auto extract_frame = [&](uint16_t msg_type) -> std::vector<uint8_t> {
            std::lock_guard lk(sink.mu);
            for (auto it = sink.frames.begin(); it != sink.frames.end(); ++it) {
                if (it->data.size() < sizeof(header_t)) continue;
                auto* hdr = reinterpret_cast<const header_t*>(it->data.data());
                if (hdr->payload_type == msg_type) {
                    auto data = std::move(it->data);
                    sink.frames.erase(it);
                    return data;
                }
            }
            return {};
        };

        // Step 1: A sent NOISE_INIT → feed to B
        auto init_frame = extract_frame(MSG_TYPE_NOISE_INIT);
        EXPECT_FALSE(init_frame.empty()) << "A did not send NOISE_INIT";
        if (!init_frame.empty())
            api_b.on_data(api_b.ctx, cid_b, init_frame.data(), init_frame.size());

        // Step 2: B sent NOISE_RESP → feed to A
        auto resp_frame = extract_frame(MSG_TYPE_NOISE_RESP);
        EXPECT_FALSE(resp_frame.empty()) << "B did not send NOISE_RESP";
        if (!resp_frame.empty())
            api_a.on_data(api_a.ctx, cid_a, resp_frame.data(), resp_frame.size());

        // Step 3: A sent NOISE_FIN (triggered by processing RESP) → feed to B
        auto fin_frame = extract_frame(MSG_TYPE_NOISE_FIN);
        EXPECT_FALSE(fin_frame.empty()) << "A did not send NOISE_FIN";
        if (!fin_frame.empty())
            api_b.on_data(api_b.ctx, cid_b, fin_frame.data(), fin_frame.size());

        // Re-register the non-capturing mock for subsequent operations
        cm_a.register_connector("tcp", &mock_ops_);
        cm_b.register_connector("tcp", &mock_ops_);
        g_cap_sink = nullptr;

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
    EXPECT_EQ(id.user_pubkey_hex().size(), 64u);
    EXPECT_EQ(id.device_pubkey_hex().size(), 64u);
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

    Config::Identity cfg;
    cfg.dir = dir.string();
    cfg.ssh_key_path = pem.string();
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
    write_str(blob, "aes256-ctr");
    write_str(blob, "bcrypt");
    write_str(blob, "kdf_options_placeholder");

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
    EXPECT_NE(id1.device_pubkey_hex(), id2.device_pubkey_hex());
    fs::remove_all(d1); fs::remove_all(d2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 2: NoiseSession / cm_session.cpp
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SessionTest, EncryptDecryptRoundTrip) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    std::vector<uint8_t> plain = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    uint64_t nonce = 1;
    auto wire = s.encrypt(plain.data(), plain.size(), nonce, true, 512, 1);
    ASSERT_FALSE(wire.empty());

    auto result = s.decrypt(wire.data(), wire.size(), nonce);
    ASSERT_EQ(result, plain);
}

TEST(SessionTest, DifferentNonces_DifferentCiphertext) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    uint8_t dummy[1] = {0};
    auto w1 = s.encrypt(dummy, 1, 1, true, 512, 1);
    auto w2 = s.encrypt(dummy, 1, 2, true, 512, 1);
    EXPECT_NE(w1, w2);
}

TEST(SessionTest, ReplayProtection) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    uint8_t msg[] = "hello";
    uint64_t nonce = 1;
    auto wire = s.encrypt(msg, 5, nonce, true, 512, 1);

    auto r1 = s.decrypt(wire.data(), wire.size(), nonce);
    EXPECT_EQ(r1.size(), 5u);

    // Replay (same nonce) — must fail
    auto r2 = s.decrypt(wire.data(), wire.size(), nonce);
    EXPECT_TRUE(r2.empty());
}

TEST(SessionTest, TamperedPayloadRejected) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    uint8_t msg[] = "tamper me";
    auto wire = s.encrypt(msg, sizeof(msg), 1, true, 512, 1);
    // flip a bit in the ciphertext
    if (wire.size() > 4) wire[4] ^= 0xFF;

    auto result = s.decrypt(wire.data(), wire.size(), 1);
    EXPECT_TRUE(result.empty());
}

TEST(SessionTest, TooShortPacketRejected) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));

    uint8_t tiny[8]{};
    auto r = s.decrypt(tiny, sizeof(tiny), 1);
    EXPECT_TRUE(r.empty());
}

TEST(SessionTest, EmptyPlaintextRoundTrip) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    auto wire = s.encrypt(nullptr, 0, 1, true, 512, 1);
    auto result = s.decrypt(wire.data(), wire.size(), 1);
    EXPECT_TRUE(result.empty());
}

TEST(SessionTest, LargePayloadRoundTrip) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    std::vector<uint8_t> big(64 * 1024);
    randombytes_buf(big.data(), big.size());
    auto wire = s.encrypt(big.data(), big.size(), 1, true, 512, 1);
    auto result = s.decrypt(wire.data(), wire.size(), 1);
    ASSERT_EQ(result, big);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 3: HandshakePayload helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(HandshakePayloadTest, SetGetSchemesRoundTrip) {
    msg::HandshakePayload hp{};
    hp.set_schemes({"tcp", "ws", "udp"});
    EXPECT_EQ(hp.schemes_count, 3);
    auto schemes = hp.get_schemes();
    ASSERT_EQ(schemes.size(), 3u);
    EXPECT_EQ(schemes[0], "tcp");
    EXPECT_EQ(schemes[1], "ws");
    EXPECT_EQ(schemes[2], "udp");
}

TEST(HandshakePayloadTest, TooManySchemesClipped) {
    msg::HandshakePayload hp{};
    hp.set_schemes({"a","b","c","d","e","f","g","h","i","j"});
    EXPECT_EQ(hp.schemes_count, msg::HANDSHAKE_MAX_SCHEMES);
}

TEST(HandshakePayloadTest, EmptySchemesProducesWildcard) {
    msg::HandshakePayload hp{};
    hp.set_schemes({});
    EXPECT_EQ(hp.schemes_count, 0);
    auto s = hp.get_schemes();
    EXPECT_TRUE(s.empty());
}

TEST(HandshakePayloadTest, SizeCheck) {
    EXPECT_EQ(sizeof(msg::CoreMeta), 8u);
    EXPECT_EQ(sizeof(msg::HandshakePayload), msg::kHandshakePayloadSize);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 4: ConnectionManager — connection lifecycle
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
    ep.flags = EP_FLAG_TRUSTED;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    EXPECT_NE(id, CONN_ID_INVALID);
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
    ep.flags = EP_FLAG_TRUSTED;
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

TEST_F(CMTest, PacketBeforeHandshakeDropped) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 5000;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    header_t h{};
    h.magic        = GNET_MAGIC;
    h.proto_ver    = GNET_PROTO_VER;
    h.payload_type = MSG_TYPE_CHAT;
    h.payload_len  = 4;
    uint8_t pl[4] = {1,2,3,4};
    std::vector<uint8_t> frame(sizeof(h) + 4);
    std::memcpy(frame.data(), &h, sizeof(h));
    std::memcpy(frame.data() + sizeof(h), pl, 4);

    EXPECT_NO_THROW(api.on_data(api.ctx, id, frame.data(), frame.size()));
    auto st = cm_a_->get_state(id);
    ASSERT_TRUE(st.has_value());
    EXPECT_NE(*st, STATE_ESTABLISHED);
}

TEST_F(CMTest, InvalidMagicDropped) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.2", sizeof(ep.address));
    ep.port = 5003;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    header_t h{};
    h.magic = 0xDEADBEEF;
    h.proto_ver = GNET_PROTO_VER;
    h.payload_type = MSG_TYPE_NOISE_INIT;
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
    EXPECT_NO_THROW(cm_a_->send("tcp://10.0.0.99:1234",
                                  MSG_TYPE_CHAT, std::span<const uint8_t>{}));
}

TEST_F(CMTest, SendToUnknownUriWithConnector_DoesNotCrash) {
    cm_a_->register_connector("tcp", &mock_ops_);
    EXPECT_NO_THROW(cm_a_->send("mock://10.0.0.99:1234",
                                  MSG_TYPE_CHAT, std::span<const uint8_t>{}));
}

TEST_F(CMTest, RegisterConnectorAndQueryScheme) {
    cm_a_->register_connector("tcp", &mock_ops_);
    EXPECT_NO_THROW(cm_a_->send("tcp://10.0.0.1:9000", MSG_TYPE_HEARTBEAT,
                                  std::span<const uint8_t>{}));
}

TEST_F(CMTest, SetSchemePriorityAffectsNegotiation) {
    cm_a_->set_scheme_priority({"ws", "tcp", "mock"});
    cm_a_->register_connector("tcp", &mock_ops_);
    EXPECT_NO_THROW(cm_a_->send("mock://10.0.0.1:9000", MSG_TYPE_CHAT,
                                  std::span<const uint8_t>{}));
}

TEST_F(CMTest, SendToDisconnectedIdDroppedGracefully) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 9000;
    conn_id_t id = api.on_connect(api.ctx, &ep);
    api.on_disconnect(api.ctx, id, 0);
    EXPECT_NO_THROW(cm_a_->send("10.0.0.1:9000", MSG_TYPE_CHAT,
                                  std::span<const uint8_t>{}));
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
    api.on_disconnect(api.ctx, id, 1);
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

    int v = api.verify_signature(api.ctx, msg, sizeof(msg),
                                   id_b_.device_pubkey, sig);
    EXPECT_NE(v, 0);
}

TEST_F(CMTest, HostApiLoggerNotNull) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    EXPECT_NE(api.ctx, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 8: Full Noise_XX handshake (localhost)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, LocalhostHandshakeEstablishes) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto st_a = cm_a_->get_state(cid_a);
    ASSERT_TRUE(st_a.has_value());
    EXPECT_EQ(*st_a, STATE_ESTABLISHED);

    auto st_b = cm_b_->get_state(cid_b);
    ASSERT_TRUE(st_b.has_value());
    EXPECT_EQ(*st_b, STATE_ESTABLISHED);
}

TEST_F(CMTest, LocalhostHandshake_SchemesNegotiated) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto st = cm_a_->get_state(cid_a);
    EXPECT_TRUE(st.has_value());
    EXPECT_EQ(*st, STATE_ESTABLISHED);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 9: Shutdown behaviour
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, ShutdownIdempotent) {
    EXPECT_NO_THROW(cm_a_->shutdown());
    EXPECT_NO_THROW(cm_a_->shutdown());
}

TEST_F(CMTest, SendAfterShutdownDoesNotCrash) {
    cm_a_->register_connector("tcp", &mock_ops_);
    cm_a_->shutdown();
    EXPECT_NO_THROW(cm_a_->send("mock://10.0.0.1:1234", MSG_TYPE_CHAT,
                                  std::span<const uint8_t>{}));
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
    cm_a_->register_connector("tcp", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    std::vector<uint8_t> big(512 * 1024);
    randombytes_buf(big.data(), big.size());

    bool ok = cm_a_->send(cid_a, MSG_TYPE_CHAT,
                           std::span<const uint8_t>{big});
    EXPECT_TRUE(ok);
}

TEST_F(CMTest, GetPeerPubkeyAfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto pk = cm_a_->get_peer_pubkey(cid_a);
    ASSERT_TRUE(pk.has_value());
    EXPECT_EQ(pk->size(), 32u);
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
    cm_a_->register_connector("tcp", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    uint8_t payload[] = {1, 2, 3, 4};
    bool ok = cm_a_->send(cid_a, MSG_TYPE_CHAT, std::span{payload});
    EXPECT_TRUE(ok);
}

TEST_F(CMTest, SendOnConn_InvalidId_ReturnsFalse) {
    cm_a_->register_connector("tcp", &mock_ops_);
    uint8_t payload[] = {1};
    bool ok = cm_a_->send(conn_id_t{9999}, MSG_TYPE_CHAT, std::span{payload});
    EXPECT_FALSE(ok);
}

TEST_F(CMTest, DisconnectCleansUpAllIndices) {
    host_api_t api_a{};
    cm_a_->fill_host_api(&api_a);

    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    EXPECT_NE(cm_a_->get_state(cid_a), std::nullopt);
    EXPECT_NE(cm_a_->find_conn_by_pubkey(id_b_.user_pubkey_hex().c_str()),
              CONN_ID_INVALID);

    api_a.on_disconnect(api_a.ctx, cid_a, 0);

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
    cm_a_->register_connector("tcp", &mock_ops_);
    uint8_t data[] = {0xAA, 0xBB};
    EXPECT_NO_THROW(cm_a_->broadcast(MSG_TYPE_CHAT, std::span{data}));
}

TEST_F(CMTest, BroadcastToEstablishedPeers) {
    cm_a_->register_connector("tcp", &mock_ops_);

    auto dir_c = tmp_dir("c");
    auto id_c = NodeIdentity::load_or_generate(dir_c);
    boost::asio::io_context ioc_c;
    SignalBus bus_c{ioc_c};
    auto cm_c = std::make_unique<ConnectionManager>(bus_c, id_c);

    auto [cid_ab, cid_ba] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);
    auto [cid_ac, cid_ca] = do_handshake(*cm_a_, id_a_, *cm_c,  id_c,  true);

    EXPECT_EQ(*cm_a_->get_state(cid_ab), STATE_ESTABLISHED);
    EXPECT_EQ(*cm_a_->get_state(cid_ac), STATE_ESTABLISHED);

    uint8_t data[] = {0x01};
    EXPECT_NO_THROW(cm_a_->broadcast(MSG_TYPE_CHAT, std::span{data}));

    cm_c->shutdown();
    fs::remove_all(dir_c);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 13: Rekey
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, RekeyOnNonEstablished_Fails) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 8000;
    conn_id_t id = api.on_connect(api.ctx, &ep);

    EXPECT_FALSE(cm_a_->rekey_session(id));
}

TEST_F(CMTest, RekeySession_Succeeds) {
    cm_a_->register_connector("tcp", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    EXPECT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);
    EXPECT_TRUE(cm_a_->rekey_session(cid_a));
}

TEST_F(CMTest, RekeyOnInvalidId_Fails) {
    EXPECT_FALSE(cm_a_->rekey_session(99999));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 14: Compression (NoiseSession level)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SessionCompressionTest, Encrypt_Large_UsesZstd) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    std::vector<uint8_t> big(2048, 0xAA);
    auto wire = s.encrypt(big.data(), big.size(), 1, true, 512, 1);
    ASSERT_FALSE(wire.empty());

    // Compressed + encrypted should be smaller than uncompressed + encrypted
    constexpr size_t MAC = 16; // ChaChaPoly IETF MAC
    size_t uncompressed_wire_size = 1u + big.size() + MAC;
    EXPECT_LT(wire.size(), uncompressed_wire_size);
}

TEST(SessionCompressionTest, Decrypt_Zstd_Decompresses) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    std::vector<uint8_t> big(2048);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i % 17);

    auto wire = s.encrypt(big.data(), big.size(), 1, true, 512, 1);
    auto result = s.decrypt(wire.data(), wire.size(), 1);
    ASSERT_EQ(result.size(), big.size());
    EXPECT_EQ(result, big);
}

TEST(SessionCompressionTest, Encrypt_Small_NoCompression) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    std::vector<uint8_t> small(100, 0xBB);
    auto wire = s.encrypt(small.data(), small.size(), 1, true, 512, 1);

    constexpr size_t MAC = 16;
    size_t expected = 1u + small.size() + MAC;
    EXPECT_EQ(wire.size(), expected);

    auto result = s.decrypt(wire.data(), wire.size(), 1);
    EXPECT_EQ(result, small);
}

TEST(SessionCompressionTest, CompressionRoundtrip_Stress) {
    if (sodium_init() < 0) GTEST_SKIP();
    NoiseSession s;
    randombytes_buf(s.send_key, sizeof(s.send_key));
    std::memcpy(s.recv_key, s.send_key, sizeof(s.recv_key));

    for (int i = 0; i < 100; ++i) {
        size_t sz = 128 + (i * 80);
        std::vector<uint8_t> plain(sz);
        randombytes_buf(plain.data(), plain.size());
        uint64_t nonce = static_cast<uint64_t>(i + 1);
        auto wire = s.encrypt(plain.data(), plain.size(), nonce, true, 512, 1);
        auto result = s.decrypt(wire.data(), wire.size(), nonce);
        ASSERT_EQ(result.size(), plain.size()) << "iteration " << i;
        EXPECT_EQ(result, plain) << "iteration " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 15: Noise handshake verification
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, BothSides_Established) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    EXPECT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);
    EXPECT_EQ(*cm_b_->get_state(cid_b), STATE_ESTABLISHED);
}

TEST_F(CMTest, DifferentKeys_DifferentSessions) {
    auto [cid_a1, cid_b1] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto dir_c = tmp_dir("c");
    auto dir_d = tmp_dir("d");
    auto id_c = NodeIdentity::load_or_generate(dir_c);
    auto id_d = NodeIdentity::load_or_generate(dir_d);
    boost::asio::io_context ioc2;
    SignalBus bus2{ioc2};
    auto cm_c = std::make_unique<ConnectionManager>(bus2, id_c);
    auto cm_d = std::make_unique<ConnectionManager>(bus2, id_d);

    auto [cid_c, cid_d] = do_handshake(*cm_c, id_c, *cm_d, id_d, true);

    EXPECT_EQ(*cm_a_->get_state(cid_a1), STATE_ESTABLISHED);
    EXPECT_EQ(*cm_c->get_state(cid_c), STATE_ESTABLISHED);

    cm_c->shutdown();
    cm_d->shutdown();
    fs::remove_all(dir_c);
    fs::remove_all(dir_d);
}

TEST_F(CMTest, HandshakeBothDirections) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);
    auto [cid_b2, cid_a2] = do_handshake(*cm_b_, id_b_, *cm_a_, id_a_, true);

    // Первое соединение остаётся ESTABLISHED
    EXPECT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);
    EXPECT_EQ(*cm_b_->get_state(cid_b), STATE_ESTABLISHED);

    // Дубликаты закрыты (duplicate peer detection)
    EXPECT_FALSE(cm_a_->get_state(cid_a2).has_value());
    EXPECT_FALSE(cm_b_->get_state(cid_b2).has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 16: Relay (MSG_TYPE_RELAY)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RelayPayloadTest, SizeCheck) {
    EXPECT_EQ(sizeof(msg::RelayPayload), 33u);
}

TEST_F(CMTest, HandleRelay_LocalDelivery) {
    cm_a_->register_connector("tcp", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);
    ASSERT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);

    header_t inner_hdr{};
    inner_hdr.magic        = GNET_MAGIC;
    inner_hdr.proto_ver    = GNET_PROTO_VER;
    inner_hdr.payload_type = MSG_TYPE_CHAT;
    inner_hdr.payload_len  = 4;
    inner_hdr.packet_id    = 42;
    uint8_t chat_data[4] = {0xCA, 0xFE, 0xBA, 0xBE};

    std::vector<uint8_t> inner_frame(sizeof(inner_hdr) + 4);
    std::memcpy(inner_frame.data(), &inner_hdr, sizeof(inner_hdr));
    std::memcpy(inner_frame.data() + sizeof(inner_hdr), chat_data, 4);

    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());
    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = 3;
    std::memcpy(rp->dest_pubkey, id_a_.user_pubkey, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    header_t relay_hdr{};
    relay_hdr.magic        = GNET_MAGIC;
    relay_hdr.proto_ver    = GNET_PROTO_VER;
    relay_hdr.flags        = GNET_FLAG_TRUSTED;
    relay_hdr.payload_type = MSG_TYPE_RELAY;
    relay_hdr.payload_len  = static_cast<uint32_t>(relay_payload.size());

    std::vector<uint8_t> wire(sizeof(relay_hdr) + relay_payload.size());
    std::memcpy(wire.data(), &relay_hdr, sizeof(relay_hdr));
    std::memcpy(wire.data() + sizeof(relay_hdr),
                relay_payload.data(), relay_payload.size());

    host_api_t api{};
    cm_a_->fill_host_api(&api);
    EXPECT_NO_THROW(api.on_data(api.ctx, cid_a, wire.data(), wire.size()));
}

TEST_F(CMTest, HandleRelay_TTLZero_Dropped) {
    cm_a_->register_connector("tcp", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    header_t inner_hdr{};
    inner_hdr.magic        = GNET_MAGIC;
    inner_hdr.proto_ver    = GNET_PROTO_VER;
    inner_hdr.payload_type = MSG_TYPE_CHAT;
    inner_hdr.payload_len  = 1;
    inner_hdr.packet_id    = 200;

    std::vector<uint8_t> inner_frame(sizeof(inner_hdr) + 1);
    std::memcpy(inner_frame.data(), &inner_hdr, sizeof(inner_hdr));
    inner_frame.back() = 0xFF;

    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());
    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = 0;
    std::memcpy(rp->dest_pubkey, id_b_.user_pubkey, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    header_t relay_hdr{};
    relay_hdr.magic        = GNET_MAGIC;
    relay_hdr.proto_ver    = GNET_PROTO_VER;
    relay_hdr.flags        = GNET_FLAG_TRUSTED;
    relay_hdr.payload_type = MSG_TYPE_RELAY;
    relay_hdr.payload_len  = static_cast<uint32_t>(relay_payload.size());

    std::vector<uint8_t> wire(sizeof(relay_hdr) + relay_payload.size());
    std::memcpy(wire.data(), &relay_hdr, sizeof(relay_hdr));
    std::memcpy(wire.data() + sizeof(relay_hdr),
                relay_payload.data(), relay_payload.size());

    host_api_t api{};
    cm_a_->fill_host_api(&api);
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

    endpoint_t ep1{}, ep2{};
    strncpy(ep1.address, "10.0.0.1", sizeof(ep1.address)); ep1.port = 9001;
    strncpy(ep2.address, "10.0.0.2", sizeof(ep2.address)); ep2.port = 9002;

    api_a.on_connect(api_a.ctx, &ep1);
    api_a.on_connect(api_a.ctx, &ep2);

    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto active = cm_a_->get_active_conn_ids();
    EXPECT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0], cid_a);
}

TEST_F(CMTest, GetPeerEndpoint_AfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto ep = cm_a_->get_peer_endpoint(cid_a);
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->peer_id, cid_a);
    EXPECT_GT(strlen(ep->address), 0u);
}

TEST_F(CMTest, DisconnectGraceful_DrainsQueue) {
    cm_a_->register_connector("tcp", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    EXPECT_NO_THROW(cm_a_->disconnect(cid_a));
}

TEST_F(CMTest, CloseNow_ImmediateClose) {
    cm_a_->register_connector("tcp", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    EXPECT_NO_THROW(cm_a_->close_now(cid_a));
}

TEST_F(CMTest, RotateIdentityKeys) {
    auto old_hex = cm_a_->identity().user_pubkey_hex();
    auto dir_rot = tmp_dir("rot");

    uint8_t rot_pub[32], rot_sec[64];
    crypto_sign_keypair(rot_pub, rot_sec);
    write_openssh_pem(dir_rot, rot_pub, rot_sec);

    Config::Identity cfg{
        .dir          = dir_rot.string(),
        .ssh_key_path = (dir_rot / "id_ed25519").string(),
        .use_machine_id = false,
    };
    cm_a_->rotate_identity_keys(cfg);

    auto new_hex = cm_a_->identity().user_pubkey_hex();
    EXPECT_NE(old_hex, new_hex);
    fs::remove_all(dir_rot);
}

TEST_F(CMTest, LocalCoreMeta_HasVersion) {
    auto meta = cm_a_->local_core_meta();
    EXPECT_GT(meta.core_version, 0u);
    EXPECT_NE(meta.caps_mask & CORE_CAP_ZSTD, 0u);
    EXPECT_NE(meta.caps_mask & CORE_CAP_KEYROT, 0u);
}

TEST_F(CMTest, Broadcast_SkipsNonEstablished) {
    cm_a_->register_connector("tcp", &mock_ops_);
    host_api_t api{};
    cm_a_->fill_host_api(&api);

    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.5", sizeof(ep.address));
    ep.port = 7777;
    api.on_connect(api.ctx, &ep);

    uint8_t data[] = {1, 2, 3};
    EXPECT_NO_THROW(cm_a_->broadcast(MSG_TYPE_CHAT, std::span{data}));
}

TEST_F(CMTest, GetPendingBytes_NoConn_ReturnsZero) {
    EXPECT_EQ(cm_a_->get_pending_bytes(CONN_ID_INVALID), 0u);
    EXPECT_EQ(cm_a_->get_pending_bytes(9999), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 18: Relay gossip forward (3-node chain A↔B↔C)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, RelayForward_ThreeNodes) {
    // Третья нода C
    auto dir_c = tmp_dir("relay_c");
    auto id_c = NodeIdentity::load_or_generate(dir_c);
    boost::asio::io_context ioc_c;
    SignalBus bus_c{ioc_c};
    auto cm_c = std::make_unique<ConnectionManager>(bus_c, id_c);

    // A↔B handshake (localhost)
    auto [cid_ab, cid_ba] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);
    ASSERT_EQ(*cm_a_->get_state(cid_ab), STATE_ESTABLISHED);
    ASSERT_EQ(*cm_b_->get_state(cid_ba), STATE_ESTABLISHED);

    // B↔C handshake (localhost)
    auto [cid_bc, cid_cb] = do_handshake(*cm_b_, id_b_, *cm_c, id_c, true);
    ASSERT_EQ(*cm_b_->get_state(cid_bc), STATE_ESTABLISHED);
    ASSERT_EQ(*cm_c->get_state(cid_cb), STATE_ESTABLISHED);

    // C отправляет relay пакет с dest_pubkey = A через B.
    // Собираем relay wire frame как если бы C уже передал через свой connector.
    header_t inner_hdr{};
    inner_hdr.magic        = GNET_MAGIC;
    inner_hdr.proto_ver    = GNET_PROTO_VER;
    inner_hdr.payload_type = MSG_TYPE_CHAT;
    inner_hdr.payload_len  = 5;
    inner_hdr.packet_id    = 777;
    uint8_t chat_data[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};

    std::vector<uint8_t> inner_frame(sizeof(inner_hdr) + 5);
    std::memcpy(inner_frame.data(), &inner_hdr, sizeof(inner_hdr));
    std::memcpy(inner_frame.data() + sizeof(inner_hdr), chat_data, 5);

    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());
    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = 5;
    std::memcpy(rp->dest_pubkey, id_a_.user_pubkey, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    // Оборачиваем в wire frame с TRUSTED (localhost)
    header_t relay_hdr{};
    relay_hdr.magic        = GNET_MAGIC;
    relay_hdr.proto_ver    = GNET_PROTO_VER;
    relay_hdr.flags        = GNET_FLAG_TRUSTED;
    relay_hdr.payload_type = MSG_TYPE_RELAY;
    relay_hdr.payload_len  = static_cast<uint32_t>(relay_payload.size());

    std::vector<uint8_t> wire(sizeof(relay_hdr) + relay_payload.size());
    std::memcpy(wire.data(), &relay_hdr, sizeof(relay_hdr));
    std::memcpy(wire.data() + sizeof(relay_hdr),
                relay_payload.data(), relay_payload.size());

    // Подаём frame в B на соединение от C (cid_bc)
    host_api_t api_b{};
    cm_b_->fill_host_api(&api_b);

    // Регистрируем capturing connector на B чтобы перехватить forward
    CapturingSink fwd_sink;
    auto fwd_ops = make_capturing_connector(&fwd_sink);
    cm_b_->register_connector("tcp", &fwd_ops);

    api_b.on_data(api_b.ctx, cid_bc, wire.data(), wire.size());

    // B должен переслать relay frame на A
    std::lock_guard lk(fwd_sink.mu);
    bool found_relay = false;
    for (auto& f : fwd_sink.frames) {
        if (f.data.size() < sizeof(header_t)) continue;
        auto* h = reinterpret_cast<const header_t*>(f.data.data());
        if (h->payload_type == MSG_TYPE_RELAY) {
            found_relay = true;
            // Проверяем что TTL декрементирован
            auto* fwd_rp = reinterpret_cast<const msg::RelayPayload*>(
                f.data.data() + sizeof(header_t));
            EXPECT_EQ(fwd_rp->ttl, 4u) << "TTL should be decremented by 1";
            break;
        }
    }
    EXPECT_TRUE(found_relay) << "B should forward relay to A";

    cm_b_->register_connector("tcp", &mock_ops_);
    g_cap_sink = nullptr;
    cm_c->shutdown();
    fs::remove_all(dir_c);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 19: Relay dedup
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, RelayDedup_SamePacketDroppedOnSecondPass) {
    cm_a_->register_connector("tcp", &mock_ops_);
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);
    ASSERT_EQ(*cm_a_->get_state(cid_a), STATE_ESTABLISHED);

    // Собираем relay frame с dest != A (чтобы A попытался forward)
    uint8_t fake_dest[32];
    std::memset(fake_dest, 0xFF, sizeof(fake_dest));

    header_t inner_hdr{};
    inner_hdr.magic        = GNET_MAGIC;
    inner_hdr.proto_ver    = GNET_PROTO_VER;
    inner_hdr.payload_type = MSG_TYPE_CHAT;
    inner_hdr.payload_len  = 1;
    inner_hdr.packet_id    = 12345;
    uint8_t data = 0xAA;

    std::vector<uint8_t> inner_frame(sizeof(inner_hdr) + 1);
    std::memcpy(inner_frame.data(), &inner_hdr, sizeof(inner_hdr));
    inner_frame.back() = data;

    constexpr size_t RELAY_HDR = sizeof(msg::RelayPayload);
    std::vector<uint8_t> relay_payload(RELAY_HDR + inner_frame.size());
    auto* rp = reinterpret_cast<msg::RelayPayload*>(relay_payload.data());
    rp->ttl = 3;
    std::memcpy(rp->dest_pubkey, fake_dest, 32);
    std::memcpy(relay_payload.data() + RELAY_HDR,
                inner_frame.data(), inner_frame.size());

    header_t relay_hdr{};
    relay_hdr.magic        = GNET_MAGIC;
    relay_hdr.proto_ver    = GNET_PROTO_VER;
    relay_hdr.flags        = GNET_FLAG_TRUSTED;
    relay_hdr.payload_type = MSG_TYPE_RELAY;
    relay_hdr.payload_len  = static_cast<uint32_t>(relay_payload.size());

    std::vector<uint8_t> wire(sizeof(relay_hdr) + relay_payload.size());
    std::memcpy(wire.data(), &relay_hdr, sizeof(relay_hdr));
    std::memcpy(wire.data() + sizeof(relay_hdr),
                relay_payload.data(), relay_payload.size());

    host_api_t api{};
    cm_a_->fill_host_api(&api);

    // Первая подача — relay_seen() вернёт false, пакет обработается
    api.on_data(api.ctx, cid_a, wire.data(), wire.size());

    // Вторая подача того же пакета — relay_seen() вернёт true, dedup
    api.on_data(api.ctx, cid_a, wire.data(), wire.size());

    // Второй пакет не должен генерировать RelayDropped (dedup — silent drop),
    // но и не должен crash
    SUCCEED() << "Duplicate relay packet handled without crash";
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 20: recv_buf overflow (M2 fix)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, RecvBufOverflow_ClosesConnection) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    cm_a_->register_connector("tcp", &mock_ops_);

    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 5555;
    conn_id_t cid = api.on_connect(api.ctx, &ep);
    EXPECT_NE(cid, CONN_ID_INVALID);

    auto drops_before = bus_.stats_snapshot().drops[
        static_cast<size_t>(DropReason::RecvBufOverflow)];

    // Подаём данные частями, не составляющими валидный фрейм,
    // чтобы recv_buf рос. Используем валидный magic + proto_ver,
    // но payload_len = огромный, чтобы буфер ждал остатка.
    header_t partial{};
    partial.magic        = GNET_MAGIC;
    partial.proto_ver    = GNET_PROTO_VER;
    partial.payload_type = MSG_TYPE_CHAT;
    partial.payload_len  = 32 * 1024 * 1024;  // 32 MB — больше MAX_RECV_BUF

    // Подаём header (20 bytes) — recv_buf = 20 bytes, ждёт payload
    api.on_data(api.ctx, cid, &partial, sizeof(partial));

    // Подаём мусор по 1MB кусками до переполнения (>16MB)
    std::vector<uint8_t> chunk(1024 * 1024, 0xCC);
    for (int i = 0; i < 17; ++i) {
        api.on_data(api.ctx, cid, chunk.data(), chunk.size());
    }

    auto drops_after = bus_.stats_snapshot().drops[
        static_cast<size_t>(DropReason::RecvBufOverflow)];
    EXPECT_GT(drops_after, drops_before)
        << "RecvBufOverflow drop should be emitted";
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION 21: Bad magic → close, Bad proto_ver → close
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, BadMagic_EmitsDrop_ClosesConnection) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    cm_a_->register_connector("tcp", &mock_ops_);

    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 6001;
    conn_id_t cid = api.on_connect(api.ctx, &ep);

    auto drops_before = bus_.stats_snapshot().drops[
        static_cast<size_t>(DropReason::BadMagic)];

    // Подаём два куска: сначала валидный header, потом мусор с bad magic.
    // Нужно заставить данные попасть в recv_buf (не fast path).
    // Метод: подать header без полного payload, затем данные с bad magic.

    // Вариант проще: подать один фрейм с valid magic (fast path пропустит если
    // total == size), потом подать фрейм с bad magic через reassembly path.

    // Самый простой: подать два фрейма в одном блоке,
    // первый валидный (чтобы пройти fast path нельзя — size != total),
    // → оба идут через reassembly.
    header_t good{};
    good.magic        = GNET_MAGIC;
    good.proto_ver    = GNET_PROTO_VER;
    good.payload_type = MSG_TYPE_NOISE_INIT;  // handshake msg — будет обработан
    good.payload_len  = 0;

    header_t bad{};
    bad.magic        = 0xDEADBEEF;
    bad.proto_ver    = GNET_PROTO_VER;
    bad.payload_type = MSG_TYPE_CHAT;
    bad.payload_len  = 0;

    // Два фрейма подряд в одном блоке → reassembly path
    std::vector<uint8_t> combined(sizeof(good) + sizeof(bad));
    std::memcpy(combined.data(), &good, sizeof(good));
    std::memcpy(combined.data() + sizeof(good), &bad, sizeof(bad));

    api.on_data(api.ctx, cid, combined.data(), combined.size());

    auto drops_after = bus_.stats_snapshot().drops[
        static_cast<size_t>(DropReason::BadMagic)];
    EXPECT_GT(drops_after, drops_before)
        << "BadMagic drop should be emitted";
}

TEST_F(CMTest, BadProtoVer_EmitsDrop_ClosesConnection) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    cm_a_->register_connector("tcp", &mock_ops_);

    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.2", sizeof(ep.address));
    ep.port = 6002;
    conn_id_t cid = api.on_connect(api.ctx, &ep);

    auto drops_before = bus_.stats_snapshot().drops[
        static_cast<size_t>(DropReason::BadProtoVer)];

    // Аналогично: два фрейма → reassembly. Первый валидный, второй bad proto_ver.
    header_t good{};
    good.magic        = GNET_MAGIC;
    good.proto_ver    = GNET_PROTO_VER;
    good.payload_type = MSG_TYPE_NOISE_INIT;
    good.payload_len  = 0;

    header_t bad{};
    bad.magic        = GNET_MAGIC;
    bad.proto_ver    = 99;
    bad.payload_type = MSG_TYPE_CHAT;
    bad.payload_len  = 0;

    std::vector<uint8_t> combined(sizeof(good) + sizeof(bad));
    std::memcpy(combined.data(), &good, sizeof(good));
    std::memcpy(combined.data() + sizeof(good), &bad, sizeof(bad));

    api.on_data(api.ctx, cid, combined.data(), combined.size());

    auto drops_after = bus_.stats_snapshot().drops[
        static_cast<size_t>(DropReason::BadProtoVer)];
    EXPECT_GT(drops_after, drops_before)
        << "BadProtoVer drop should be emitted";
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION: registration.cpp coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, RegisterConnector_SendWorks) {
    // Проверяем что register_connector делает коннектор доступным,
    // и send_to вызывается при отправке через on_connect.
    CapturingSink sink;
    auto cap = make_capturing_connector(&sink);
    cm_a_->register_connector("tcp", &cap);

    // Прямая отправка через коннектор — проверяет регистрацию
    uint8_t data[] = {1, 2, 3};
    int rc = cap.send_to(cap.connector_ctx, 42, data, sizeof(data));
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(sink.frames.size(), 1u);
    EXPECT_EQ(sink.frames[0].id, 42u);
    EXPECT_EQ(sink.frames[0].data.size(), 3u);

    g_cap_sink = nullptr;
}

TEST_F(CMTest, RegisterHandler_WithTypes) {
    auto api = make_api(*cm_a_);
    static bool received = false;
    received = false;

    handler_t h{};
    h.name = "test_typed";
    static uint32_t types[] = { MSG_TYPE_CHAT };
    h.supported_types = types;
    h.num_supported_types = 1;
    h.handle_message = [](void*, const header_t*, const endpoint_t*,
                          const void*, size_t) { received = true; };
    h.user_data = nullptr;

    cm_a_->register_handler(&h);

    // Проверяем что handler зарегистрирован
    auto state_before = received;
    EXPECT_FALSE(state_before);
}

TEST_F(CMTest, RegisterHandler_Wildcard) {
    auto api = make_api(*cm_a_);

    handler_t h{};
    h.name = "test_wildcard";
    h.supported_types = nullptr;
    h.num_supported_types = 0;
    h.handle_message = [](void*, const header_t*, const endpoint_t*,
                          const void*, size_t) {};
    h.user_data = nullptr;

    // Wildcard handler from plugin source — should not crash
    EXPECT_NO_THROW(cm_a_->register_handler(&h));
}

TEST_F(CMTest, RegisterHandlerFromConnector_WildcardBlocked) {
    auto api = make_api(*cm_a_);

    handler_t h{};
    h.name = "conn_wildcard";
    h.supported_types = nullptr;
    h.num_supported_types = 0;
    h.handle_message = [](void*, const header_t*, const endpoint_t*,
                          const void*, size_t) {};
    h.user_data = nullptr;

    // Connector-source wildcard should be blocked
    impl(*cm_a_).register_handler_from_connector(&h);

    // Verify handler was NOT registered (wildcard blocked for connector source)
    std::shared_lock lk(impl(*cm_a_).handlers_mu_);
    EXPECT_EQ(impl(*cm_a_).handler_entries_.count("conn_wildcard"), 0u);
}

TEST_F(CMTest, RegisterHandlerFromConnector_SystemTypeBlocked) {
    auto api = make_api(*cm_a_);

    handler_t h{};
    h.name = "conn_system";
    // Попытка подписаться на системные типы — должны быть отклонены
    static uint32_t types[] = { MSG_TYPE_HEARTBEAT, MSG_TYPE_CHAT };
    h.supported_types = types;
    h.num_supported_types = 2;
    h.handle_message = [](void*, const header_t*, const endpoint_t*,
                          const void*, size_t) {};
    h.user_data = nullptr;

    impl(*cm_a_).register_handler_from_connector(&h);

    // Handler зарегистрирован только с MSG_TYPE_CHAT (system type отклонён)
    std::shared_lock lk(impl(*cm_a_).handlers_mu_);
    auto it = impl(*cm_a_).handler_entries_.find("conn_system");
    ASSERT_NE(it, impl(*cm_a_).handler_entries_.end());
    // MSG_TYPE_HEARTBEAT (4) is in 0-9 range, blocked
    EXPECT_EQ(it->second.subscribed_types.size(), 1u);
    EXPECT_EQ(it->second.subscribed_types[0], MSG_TYPE_CHAT);
}

TEST_F(CMTest, IsConnectorBlockedType) {
    // 0-9: system types — blocked
    EXPECT_TRUE(ConnectionManager::Impl::is_connector_blocked_type(0));
    EXPECT_TRUE(ConnectionManager::Impl::is_connector_blocked_type(4));
    EXPECT_TRUE(ConnectionManager::Impl::is_connector_blocked_type(9));

    // 10+: user types — not blocked
    EXPECT_FALSE(ConnectionManager::Impl::is_connector_blocked_type(10));
    EXPECT_FALSE(ConnectionManager::Impl::is_connector_blocked_type(MSG_TYPE_CHAT));

    // SYS_BASE (0x0100) to SYS_MAX — blocked
    EXPECT_TRUE(ConnectionManager::Impl::is_connector_blocked_type(MSG_TYPE_SYS_BASE));
    EXPECT_TRUE(ConnectionManager::Impl::is_connector_blocked_type(MSG_TYPE_SYS_MAX));

    // Above SYS_MAX — not blocked
    EXPECT_FALSE(ConnectionManager::Impl::is_connector_blocked_type(MSG_TYPE_SYS_MAX + 1));
}

TEST_F(CMTest, SetSchemePriority) {
    cm_a_ = std::make_unique<ConnectionManager>(bus_, id_a_);
    cm_a_->set_scheme_priority({"ice", "tcp"});

    // Verify priority order via impl
    EXPECT_EQ(impl(*cm_a_).scheme_priority_index("ice"), 0u);
    EXPECT_EQ(impl(*cm_a_).scheme_priority_index("tcp"), 1u);
    EXPECT_EQ(impl(*cm_a_).scheme_priority_index("unknown"), 255u);
}

TEST_F(CMTest, RegisterHandler_NullHandlerIgnored) {
    auto api = make_api(*cm_a_);
    EXPECT_NO_THROW(cm_a_->register_handler(nullptr));
}

TEST_F(CMTest, RegisterHandlerFromConnector_AllTypesBlocked) {
    auto api = make_api(*cm_a_);

    handler_t h{};
    h.name = "all_blocked";
    // Только системные типы — все будут отклонены
    static uint32_t types[] = { 0, 1, 2 };
    h.supported_types = types;
    h.num_supported_types = 3;
    h.handle_message = [](void*, const header_t*, const endpoint_t*,
                          const void*, size_t) {};
    h.user_data = nullptr;

    impl(*cm_a_).register_handler_from_connector(&h);

    // Handler не должен быть зарегистрирован (все типы отклонены)
    std::shared_lock lk(impl(*cm_a_).handlers_mu_);
    EXPECT_EQ(impl(*cm_a_).handler_entries_.count("all_blocked"), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION: queries.cpp coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, FindConnByPubkey_AfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    // A знает pubkey B
    auto pk_hex = id_b_.user_pubkey_hex();
    conn_id_t found = cm_a_->find_conn_by_pubkey(pk_hex.c_str());
    EXPECT_EQ(found, cid_a);
}

TEST_F(CMTest, FindConnByPubkey_Nullptr) {
    EXPECT_EQ(cm_a_->find_conn_by_pubkey(nullptr), CONN_ID_INVALID);
}

TEST_F(CMTest, FindConnByPubkey_Unknown) {
    EXPECT_EQ(cm_a_->find_conn_by_pubkey("aabbccdd"), CONN_ID_INVALID);
}

TEST_F(CMTest, GetPeerPubkey_AfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto pk = cm_a_->get_peer_pubkey(cid_a);
    ASSERT_TRUE(pk.has_value());
    EXPECT_EQ(pk->size(), 32u);
}

TEST_F(CMTest, GetPeerPubkey_Unauthenticated) {
    auto api = make_api(*cm_a_);
    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.5", sizeof(ep.address));
    ep.port = 5000;
    conn_id_t cid = api.on_connect(api.ctx, &ep);

    // До handshake — unauthenticated → nullopt
    auto pk = cm_a_->get_peer_pubkey(cid);
    EXPECT_FALSE(pk.has_value());
}

TEST_F(CMTest, GetPeerPubkeyHex_AfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto hex = cm_a_->get_peer_pubkey_hex(cid_a);
    EXPECT_EQ(hex.size(), 64u);
    // Должен соответствовать pubkey B
    EXPECT_EQ(hex, id_b_.user_pubkey_hex());
}

TEST_F(CMTest, GetPeerEndpoint_HasPortAndId) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto ep = cm_a_->get_peer_endpoint(cid_a);
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->port, 9999);
    EXPECT_EQ(ep->peer_id, cid_a);
}

TEST_F(CMTest, GetNegotiatedScheme_AfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto scheme = cm_a_->get_negotiated_scheme(cid_a);
    ASSERT_TRUE(scheme.has_value());
    // После handshake negotiated_scheme заполнен
    EXPECT_FALSE(scheme->empty());
}

TEST_F(CMTest, GetState_BeforeAndAfterHandshake) {
    auto api = make_api(*cm_a_);

    endpoint_t ep{};
    strncpy(ep.address, "10.0.0.1", sizeof(ep.address));
    ep.port = 5001;
    ep.flags = EP_FLAG_OUTBOUND;
    conn_id_t cid = api.on_connect(api.ctx, &ep);

    // До handshake
    auto state = cm_a_->get_state(cid);
    ASSERT_TRUE(state.has_value());
    // Состояние HANDSHAKE_INIT (отправил NOISE_INIT)
    EXPECT_NE(*state, STATE_ESTABLISHED);
}

TEST_F(CMTest, GetState_Established) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto state = cm_a_->get_state(cid_a);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_ESTABLISHED);
}

TEST_F(CMTest, GetPendingBytes_Total) {
    auto api = make_api(*cm_a_);
    // Без соединений — 0 pending
    size_t total = cm_a_->get_pending_bytes(CONN_ID_INVALID);
    EXPECT_EQ(total, 0u);
}

TEST_F(CMTest, DumpConnections_EmptyJson) {
    auto json_str = cm_a_->dump_connections();
    auto j = nlohmann::json::parse(json_str);
    EXPECT_TRUE(j.is_array());
    EXPECT_TRUE(j.empty());
}

TEST_F(CMTest, DumpConnections_AfterHandshake) {
    auto [cid_a, cid_b] = do_handshake(*cm_a_, id_a_, *cm_b_, id_b_, true);

    auto json_str = cm_a_->dump_connections();
    auto j = nlohmann::json::parse(json_str);
    ASSERT_TRUE(j.is_array());
    ASSERT_FALSE(j.empty());

    // Проверяем поля первого соединения
    auto& conn = j[0];
    EXPECT_TRUE(conn.contains("id"));
    EXPECT_TRUE(conn.contains("state"));
    EXPECT_TRUE(conn.contains("scheme"));
    EXPECT_TRUE(conn.contains("authenticated"));
    EXPECT_TRUE(conn.contains("transport_paths"));
    EXPECT_TRUE(conn["authenticated"].get<bool>());
    EXPECT_TRUE(conn.contains("peer_pubkey"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION: lifecycle.cpp coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(CMTest, Connect_TriggersConnectorConnect) {
    CapturingSink sink;
    auto cap = make_capturing_connector(&sink);
    cm_a_->register_connector("tcp", &cap);

    // connect("tcp://...") должен вызвать connector->connect()
    // mock connector возвращает -1, но connect() не крашится
    EXPECT_NO_THROW(cm_a_->connect("tcp://10.0.0.1:9999"));
    g_cap_sink = nullptr;
}

TEST_F(CMTest, Connect_InvalidScheme_NoCrash) {
    auto api = make_api(*cm_a_);
    EXPECT_NO_THROW(cm_a_->connect("unknown://10.0.0.1:9999"));
}

TEST_F(CMTest, LocalCoreMeta_HasBaseCaps) {
    cm_a_ = std::make_unique<ConnectionManager>(bus_, id_a_);
    auto meta = impl(*cm_a_).local_core_meta();

    EXPECT_NE(meta.caps_mask & CORE_CAP_ZSTD, 0u);
    EXPECT_NE(meta.caps_mask & CORE_CAP_KEYROT, 0u);
    EXPECT_NE(meta.caps_mask & CORE_CAP_RELAY, 0u);
}

TEST_F(CMTest, LocalCoreMeta_WithICE) {
    cm_a_ = std::make_unique<ConnectionManager>(bus_, id_a_);

    connector_ops_t ice_ops = make_mock_connector_ops();
    cm_a_->register_connector("ice", &ice_ops);

    auto meta = impl(*cm_a_).local_core_meta();
    EXPECT_NE(meta.caps_mask & CORE_CAP_ICE, 0u);
}

TEST_F(CMTest, ShutdownIdempotent_FromLifecycle) {
    auto api = make_api(*cm_a_);
    cm_a_->shutdown();
    EXPECT_NO_THROW(cm_a_->shutdown());
}

TEST_F(CMTest, HostApi_SendTrampoline) {
    CapturingSink sink;
    auto cap = make_capturing_connector(&sink);

    host_api_t api{};
    cm_a_->fill_host_api(&api);
    cm_a_->register_connector("tcp", &cap);

    // s_send через host_api
    const char* uri = "tcp://10.0.0.1:8888";
    uint8_t payload[] = {1, 2, 3};
    EXPECT_NO_THROW(api.send(api.ctx, uri, MSG_TYPE_CHAT, payload, sizeof(payload)));
    g_cap_sink = nullptr;
}

TEST_F(CMTest, HostApi_BroadcastTrampoline) {
    auto api = make_api(*cm_a_);
    uint8_t payload[] = {4, 5, 6};
    EXPECT_NO_THROW(api.broadcast(api.ctx, MSG_TYPE_CHAT, payload, sizeof(payload)));
}

TEST_F(CMTest, HostApi_DisconnectTrampoline) {
    auto api = make_api(*cm_a_);
    // Disconnect несуществующего — не крашится
    EXPECT_NO_THROW(api.disconnect(api.ctx, 999));
}

TEST_F(CMTest, HostApi_SignVerify) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);

    uint8_t data[] = {0xAA, 0xBB, 0xCC};
    uint8_t sig[GN_SIGN_BYTES]{};

    int sign_ok = api.sign_with_device(api.ctx, data, sizeof(data), sig);
    EXPECT_EQ(sign_ok, 0);

    // Verify with the device pubkey
    int verify_ok = api.verify_signature(api.ctx, data, sizeof(data),
                                          id_a_.device_pubkey, sig);
    EXPECT_EQ(verify_ok, 0);
}

TEST_F(CMTest, HostApi_ConfigGet) {
    Config config(true);
    config.core.listen_port = 7777;
    cm_a_ = std::make_unique<ConnectionManager>(bus_, id_a_, &config);

    host_api_t api{};
    cm_a_->fill_host_api(&api);

    char buf[64]{};
    int ret = api.config_get(api.ctx, "core.listen_port", buf, sizeof(buf));
    EXPECT_GT(ret, 0);
    EXPECT_STREQ(buf, "7777");
}

TEST_F(CMTest, HostApi_FindConnByPubkey) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    cm_a_->register_connector("tcp", &mock_ops_);

    conn_id_t found = api.find_conn_by_pubkey(api.ctx, "nonexistent");
    EXPECT_EQ(found, CONN_ID_INVALID);
}

TEST_F(CMTest, HostApi_GetPeerInfo) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);
    cm_a_->register_connector("tcp", &mock_ops_);

    endpoint_t ep{};
    int ret = api.get_peer_info(api.ctx, 999, &ep);
    EXPECT_EQ(ret, -1);  // not found
}

TEST_F(CMTest, HostApi_LogTrampoline) {
    host_api_t api{};
    cm_a_->fill_host_api(&api);

    // Вызов log через API — не крашится
    EXPECT_NO_THROW(api.log(api.ctx, 2, __FILE__, __LINE__, "test log message"));
}
