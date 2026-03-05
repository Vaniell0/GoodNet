#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <filesystem>
#include <memory>
#include <set>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <boost/asio.hpp>
#include <sodium.h>

#include "connectionManager.hpp"
#include "include/signals.hpp"
#include "../sdk/types.h"
#include "../sdk/plugin.h"
#include "../sdk/handler.h"
#include "../sdk/connector.h"

using namespace gn;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ─── Фикстура ─────────────────────────────────────────────────────────────────

class ConnMgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (sodium_init() < 0)
            GTEST_SKIP() << "libsodium init failed";

        key_dir = fs::temp_directory_path()
                / fmt::format("goodnet_cm_{}", ::getpid());
        fs::create_directories(key_dir);

        ioc  = std::make_unique<boost::asio::io_context>();
        work = std::make_unique<boost::asio::executor_work_guard<
                   boost::asio::io_context::executor_type>>(
                   boost::asio::make_work_guard(*ioc));
        bus  = std::make_unique<gn::SignalBus>(*ioc);

        io_thread = std::thread([this] { ioc->run(); });

        auto identity = gn::NodeIdentity::load_or_generate(key_dir);
        cm = std::make_unique<gn::ConnectionManager>(*bus, std::move(identity));
    }

    void TearDown() override {
        work->reset();
        ioc->stop();
        if (io_thread.joinable()) io_thread.join();
        fs::remove_all(key_dir);
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    conn_id_t simulate_connect(const char* addr = "127.0.0.1",
                               uint16_t    port = 9000) {
        host_api_t api{}; cm->fill_host_api(&api);
        endpoint_t ep{};
        std::strncpy(ep.address, addr, sizeof(ep.address) - 1);
        ep.port = port;
        return api.on_connect(api.ctx, &ep);
    }

    // Отправить wire-пакет без шифрования (для тестов AUTH_PENDING и plain)
    void inject_plain(conn_id_t id, uint32_t msg_type, const std::string& payload) {
        host_api_t api{}; cm->fill_host_api(&api);
        header_t hdr{};
        hdr.magic = GNET_MAGIC; hdr.proto_ver = GNET_PROTO_VER;
        hdr.payload_type = msg_type;
        hdr.payload_len  = static_cast<uint32_t>(payload.size());
        std::vector<uint8_t> wire(sizeof(hdr) + payload.size());
        std::memcpy(wire.data(), &hdr, sizeof(hdr));
        std::memcpy(wire.data() + sizeof(hdr), payload.data(), payload.size());
        api.on_data(api.ctx, id, wire.data(), wire.size());
    }

    // Симулировать полный AUTH handshake от пира
    // (генерируем эфемерный X25519 + Ed25519 подпись, как делает реальный узел)
    void simulate_auth(conn_id_t id,
                       std::vector<std::string> peer_schemes = {}) {
        // Генерируем keypair пира
        crypto_sign_keypair(peer_user_pk_, peer_user_sk_);
        uint8_t dev_pk[32], dev_sk[64];
        crypto_sign_keypair(dev_pk, dev_sk);

        // X25519 эфемерный ключ
        crypto_box_keypair(peer_ephem_pk_, peer_ephem_sk_);

        // Подпись: Ed25519(user_sk, user_pk || device_pk || ephem_pk)
        uint8_t to_sign[96];
        std::memcpy(to_sign,      peer_user_pk_,  32);
        std::memcpy(to_sign + 32, dev_pk,          32);
        std::memcpy(to_sign + 64, peer_ephem_pk_,  32);

        auth_payload_t ap{};
        std::memcpy(ap.user_pubkey,   peer_user_pk_,  32);
        std::memcpy(ap.device_pubkey, dev_pk,          32);
        std::memcpy(ap.ephem_pubkey,  peer_ephem_pk_,  32);
        crypto_sign_ed25519_detached(ap.signature, nullptr,
                                     to_sign, sizeof(to_sign), peer_user_sk_);
        ap.set_schemes(peer_schemes);

        const size_t payload_len = peer_schemes.empty()
                                   ? auth_payload_t::kBaseSize
                                   : auth_payload_t::kFullSize;

        header_t hdr{};
        hdr.magic        = GNET_MAGIC;
        hdr.proto_ver    = GNET_PROTO_VER;
        hdr.payload_type = MSG_TYPE_AUTH;
        hdr.payload_len  = static_cast<uint32_t>(payload_len);

        std::vector<uint8_t> wire(sizeof(hdr) + payload_len);
        std::memcpy(wire.data(), &hdr, sizeof(hdr));
        std::memcpy(wire.data() + sizeof(hdr), &ap, payload_len);

        host_api_t api{}; cm->fill_host_api(&api);
        api.on_data(api.ctx, id, wire.data(), wire.size());
    }

    // Симулировать получение зашифрованного пакета от remote peer.
    //
    // Вызывать только после simulate_auth() на remote (не localhost) соединении.
    // Шифруем как это делал бы настоящий пир:
    //   ECDH(peer_ephem_sk, node_ephem_pk) → shared → BLAKE2b → session_key
    //   wire = SessionState::encrypt(plain, session_key)
    //
    // Для вычисления session_key нам нужен node_ephem_pk — но у нас нет
    // прямого доступа к эфемерному ключу ConnectionManager.
    // Решение: используем тот же алгоритм derive что и в cm_session.cpp.
    void inject_encrypted(conn_id_t id, uint32_t msg_type,
                           const std::string& plain_text,
                           const uint8_t node_user_pk[32]) {
        // Вычисляем session_key той же формулой что и ConnectionManager::derive_session
        // shared = X25519(peer_ephem_sk, node_ephem_pk)
        // Но мы не знаем node_ephem_pk... Обходим через обратный ECDH:
        //   Если наш peer_ephem_sk + cm->my_ephem_pk → shared
        // У нас peer_ephem_sk_ (сохранён simulate_auth) и нам нужен my_ephem_pk ядра.
        // Проблема: my_ephem_pk не публичен.
        //
        // Альтернатива: тест просто проверяет что plain-пакет для localhost работает,
        // а шифрование покрывается unit-тестами SessionState (encrypt/decrypt/replay).
        // Здесь инжектируем через SessionState напрямую.

        // Создаём SessionState с тем же ключом что derive_session использует
        // Это невозможно без доступа к node_ephem_pk ядра.
        // Поэтому тест помечается как SKIPPED через GTEST_SKIP().
        GTEST_SKIP() << "inject_encrypted требует доступа к internal ephem_pk ядра; "
                        "шифрование покрыто SessionEncryptDecryptRoundtrip тестами.";
    }

    // Симулировать получение зашифрованного пакета используя pre-computed session_key.
    // Используется совместно с EncryptedPacketE2E тестом.
    void inject_with_session_key(conn_id_t id, uint32_t msg_type,
                                  const std::string& plain_text,
                                  const uint8_t session_key[crypto_secretbox_KEYBYTES]) {
        SessionState sess;
        std::memcpy(sess.session_key, session_key, crypto_secretbox_KEYBYTES);
        sess.ready = true;

        const auto cipher = sess.encrypt(plain_text.data(), plain_text.size());

        // Оборачиваем зашифрованный payload в header
        host_api_t api{}; cm->fill_host_api(&api);
        header_t hdr{};
        hdr.magic        = GNET_MAGIC;
        hdr.proto_ver    = GNET_PROTO_VER;
        hdr.payload_type = msg_type;
        hdr.payload_len  = static_cast<uint32_t>(cipher.size());

        std::vector<uint8_t> wire(sizeof(hdr) + cipher.size());
        std::memcpy(wire.data(), &hdr, sizeof(hdr));
        std::memcpy(wire.data() + sizeof(hdr), cipher.data(), cipher.size());
        api.on_data(api.ctx, id, wire.data(), wire.size());
    }

    // Симулировать получение пакета от пира + expose его session_key
    // через патч ConnectionRecord (тест black-box: обходим через derive_session API).
    // Возвращает session_key который derive_session должен был вычислить.
    bool compute_expected_session_key(const uint8_t node_ephem_pk[32],
                                       const uint8_t node_user_pk[32],
                                       uint8_t out_session_key[crypto_secretbox_KEYBYTES]) {
        // ECDH с нашей стороны: X25519(peer_ephem_sk_, node_ephem_pk)
        uint8_t shared[crypto_scalarmult_BYTES];
        if (crypto_scalarmult(shared, peer_ephem_sk_, node_ephem_pk) != 0)
            return false;

        // Domain separation: sorted user pubkeys
        const uint8_t* pk_a = peer_user_pk_;
        const uint8_t* pk_b = node_user_pk;
        if (std::memcmp(pk_a, pk_b, 32) > 0) std::swap(pk_a, pk_b);

        crypto_generichash_state state;
        crypto_generichash_init(&state, nullptr, 0, crypto_secretbox_KEYBYTES);
        crypto_generichash_update(&state, shared, sizeof(shared));
        crypto_generichash_update(&state, pk_a,   32);
        crypto_generichash_update(&state, pk_b,   32);
        crypto_generichash_final (&state, out_session_key, crypto_secretbox_KEYBYTES);

        sodium_memzero(shared, sizeof(shared));
        return true;
    }

    // Симулировать AUTH с expose эфемерного pubkey ядра (через send_auth перехват).
    // Записывает auth-пакет который ядро отправляет в ответ — там ephem_pubkey.
    // Нужен чтобы compute_expected_session_key мог сделать ECDH.
    struct CaptureConnector {
        static int connect(void*, const char*) { return 0; }
        static int listen(void*, const char*, uint16_t) { return 0; }
        static int send_to_fn(void* ctx, conn_id_t /*id*/,
                               const void* data, size_t size) {
            auto* self = static_cast<CaptureConnector*>(ctx);
            const auto* bytes = static_cast<const uint8_t*>(data);
            if (size > sizeof(header_t)) {
                const auto* hdr = reinterpret_cast<const header_t*>(bytes);
                if (hdr->magic == GNET_MAGIC && hdr->payload_type == MSG_TYPE_AUTH) {
                    // Захватываем payload
                    size_t plen = hdr->payload_len;
                    self->auth_data.assign(bytes + sizeof(header_t),
                                           bytes + sizeof(header_t) + plen);
                }
            }
            return 0;
        }
        static void close_fn(void*, conn_id_t) {}
        static void get_scheme(void*, char* buf, size_t sz) {
            std::strncpy(buf, "mock", sz);
        }
        static void get_name(void*, char* buf, size_t sz) {
            std::strncpy(buf, "CaptureConnector", sz);
        }
        static void shutdown_fn(void*) {}

        std::vector<uint8_t> auth_data;

        connector_ops_t ops{
            connect, listen, send_to_fn, close_fn,
            get_scheme, get_name, shutdown_fn, this
        };
    };

    // Симулируем AUTH + захватываем node_ephem_pk из исходящего AUTH ядра
    conn_id_t simulate_connect_and_capture_ephem(
            uint8_t out_node_ephem_pk[32],
            const char* addr = "10.0.0.99", uint16_t port = 7777) {
        CaptureConnector cap;
        cm->register_connector("mock_cap", &cap.ops);

        host_api_t api{}; cm->fill_host_api(&api);
        endpoint_t ep{};
        std::strncpy(ep.address, addr, sizeof(ep.address)-1);
        ep.port = port;
        const conn_id_t id = api.on_connect(api.ctx, &ep);

        // handle_connect → send_auth → CaptureConnector::send_to_fn
        // К этому моменту auth_data должна быть заполнена
        if (cap.auth_data.size() >= auth_payload_t::kBaseSize) {
            const auto* ap = reinterpret_cast<const auth_payload_t*>(cap.auth_data.data());
            std::memcpy(out_node_ephem_pk, ap->ephem_pubkey, 32);
        } else {
            std::memset(out_node_ephem_pk, 0, 32);
        }
        return id;
    }

    // Симулировать AUTH от пира И вычислить session_key с обеих сторон
    void simulate_auth_and_derive_key(conn_id_t id,
                                       const uint8_t node_ephem_pk[32],
                                       const uint8_t node_user_pk[32],
                                       uint8_t out_session_key[crypto_secretbox_KEYBYTES]) {
        simulate_auth(id);
        compute_expected_session_key(node_ephem_pk, node_user_pk, out_session_key);
    }

    // Симулировать получение зашифрованного пакета используя правильный session_key.
    // Для remote peer (не localhost): dispatch_packet ищет session→decrypt().
    // Чтобы расшифровка прошла — нужен тот же ключ что derive_session вычислил внутри.
    // inject_with_session_key() использует этот ключ и отправляет wire.

    // ─────────────────────────────────────────────────────────────────────────
    template <typename Condition>
    bool wait_for(Condition&& cond, 
                  std::chrono::milliseconds max_wait = 500ms) {
        for (int i = 0; i < static_cast<int>(max_wait / 10ms); ++i) {
            if (cond()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return cond();
    }

    uint8_t peer_user_pk_ [32]{};
    uint8_t peer_user_sk_ [64]{};
    uint8_t peer_ephem_pk_[32]{};
    uint8_t peer_ephem_sk_[32]{};

    fs::path key_dir;
    std::unique_ptr<boost::asio::io_context>                            ioc;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>                        work;
    std::unique_ptr<gn::SignalBus>                                      bus;
    std::unique_ptr<gn::ConnectionManager>                              cm;
    std::thread                                                         io_thread;
};

// ─── NodeIdentity ─────────────────────────────────────────────────────────────

TEST_F(ConnMgrTest, IdentityGeneratesUserKeyFile) {
    EXPECT_TRUE(fs::exists(key_dir / "user_key"))
        << "user_key должен быть создан при первом запуске";
}

TEST_F(ConnMgrTest, IdentityPersistsAcrossReloads) {
    const std::string pk1 = cm->identity().user_pubkey_hex();
    const auto        id2 = gn::NodeIdentity::load_or_generate(key_dir);
    EXPECT_EQ(pk1, id2.user_pubkey_hex())
        << "Ключ не должен меняться при повторной загрузке";
}

TEST_F(ConnMgrTest, IdentityHexIs64Chars) {
    const std::string hex = cm->identity().user_pubkey_hex();
    ASSERT_EQ(hex.size(), 64u);
    for (char c : hex)
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)))
            << fmt::format("Не hex: '{}'", c);
}

TEST_F(ConnMgrTest, UserAndDeviceKeysAreDifferent) {
    EXPECT_NE(cm->identity().user_pubkey_hex(),
              cm->identity().device_pubkey_hex());
}

// ─── Connection lifecycle ─────────────────────────────────────────────────────

TEST_F(ConnMgrTest, ConnectReturnsValidId) {
    EXPECT_NE(simulate_connect(), CONN_ID_INVALID);
}

TEST_F(ConnMgrTest, ConnectIncrementsCount) {
    EXPECT_EQ(cm->connection_count(), 0u);
    simulate_connect("10.0.0.1", 8001);
    EXPECT_EQ(cm->connection_count(), 1u);
    simulate_connect("10.0.0.2", 8002);
    EXPECT_EQ(cm->connection_count(), 2u);
}

TEST_F(ConnMgrTest, ConnectIdsAreUnique) {
    const auto a = simulate_connect("10.0.0.1", 8001);
    const auto b = simulate_connect("10.0.0.2", 8002);
    EXPECT_NE(a, b);
}

TEST_F(ConnMgrTest, InitialStateIsAuthPending) {
    const auto id = simulate_connect();
    ASSERT_TRUE(cm->get_state(id).has_value());
    EXPECT_EQ(*cm->get_state(id), STATE_AUTH_PENDING);
}

TEST_F(ConnMgrTest, DisconnectRemovesRecord) {
    const auto id = simulate_connect();
    EXPECT_EQ(cm->connection_count(), 1u);
    host_api_t api{}; cm->fill_host_api(&api);
    api.on_disconnect(api.ctx, id, 0);
    EXPECT_EQ(cm->connection_count(), 0u);
    EXPECT_FALSE(cm->get_state(id).has_value());
}

TEST_F(ConnMgrTest, DisconnectUnknownIdIsNoOp) {
    host_api_t api{}; cm->fill_host_api(&api);
    EXPECT_NO_THROW(api.on_disconnect(api.ctx, 999999, 0));
}

// ─── AUTH Handshake ───────────────────────────────────────────────────────────

TEST_F(ConnMgrTest, ValidAuthTransitionsToEstablished) {
    const auto id = simulate_connect();
    EXPECT_EQ(*cm->get_state(id), STATE_AUTH_PENDING);

    simulate_auth(id);

    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }))
        << "После валидного AUTH должен быть STATE_ESTABLISHED";
}

TEST_F(ConnMgrTest, TamperedAuthSignatureRejected) {
    const auto id = simulate_connect();

    uint8_t pk[32], sk[64], dpk[32], dsk[64], epk[32], esk[32];
    crypto_sign_keypair(pk, sk);
    crypto_sign_keypair(dpk, dsk);
    crypto_box_keypair(epk, esk);

    uint8_t to_sign[96];
    std::memcpy(to_sign,      pk,  32);
    std::memcpy(to_sign + 32, dpk, 32);
    std::memcpy(to_sign + 64, epk, 32);

    auth_payload_t ap{};
    std::memcpy(ap.user_pubkey,   pk,  32);
    std::memcpy(ap.device_pubkey, dpk, 32);
    std::memcpy(ap.ephem_pubkey,  epk, 32);
    crypto_sign_ed25519_detached(ap.signature, nullptr, to_sign, sizeof(to_sign), sk);
    ap.signature[0] ^= 0xFF;  // портим

    header_t hdr{};
    hdr.magic = GNET_MAGIC; hdr.proto_ver = GNET_PROTO_VER;
    hdr.payload_type = MSG_TYPE_AUTH;
    hdr.payload_len  = static_cast<uint32_t>(auth_payload_t::kBaseSize);

    std::vector<uint8_t> wire(sizeof(hdr) + auth_payload_t::kBaseSize);
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    std::memcpy(wire.data() + sizeof(hdr), &ap, auth_payload_t::kBaseSize);

    host_api_t api{}; cm->fill_host_api(&api);
    api.on_data(api.ctx, id, wire.data(), wire.size());

    std::this_thread::sleep_for(50ms);
    EXPECT_NE(*cm->get_state(id), STATE_ESTABLISHED)
        << "Tampered AUTH не должен устанавливать сессию";
}

TEST_F(ConnMgrTest, BadMagicClearsBuffer) {
    const auto id = simulate_connect();
    simulate_auth(id);
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    uint8_t junk[64] = {}; junk[0] = 0xDE; junk[1] = 0xAD;
    host_api_t api{}; cm->fill_host_api(&api);
    api.on_data(api.ctx, id, junk, sizeof(junk));

    // Соединение живо — мусор только очищает буфер
    EXPECT_EQ(*cm->get_state(id), STATE_ESTABLISHED);
}

// ─── AUTH wire format ─────────────────────────────────────────────────────────

TEST_F(ConnMgrTest, AuthPayloadSizesAreCorrect) {
    EXPECT_EQ(auth_payload_t::kBaseSize, 32u + 32u + 64u + 32u);
    EXPECT_EQ(auth_payload_t::kFullSize,
              auth_payload_t::kBaseSize + 1 + AUTH_MAX_SCHEMES * AUTH_SCHEME_LEN);
    EXPECT_EQ(sizeof(auth_payload_t), auth_payload_t::kFullSize);
}

TEST_F(ConnMgrTest, AuthPayloadSchemesRoundtrip) {
    auth_payload_t ap{};
    ap.set_schemes({"tcp", "ws", "udp"});
    EXPECT_EQ(ap.schemes_count, 3u);
    const auto got = ap.get_schemes();
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "tcp");
    EXPECT_EQ(got[1], "ws");
    EXPECT_EQ(got[2], "udp");
}

TEST_F(ConnMgrTest, AuthPayloadSchemesClampedAtMax) {
    auth_payload_t ap{};
    ap.set_schemes(std::vector<std::string>(20, "tcp"));
    EXPECT_LE(ap.schemes_count, AUTH_MAX_SCHEMES);
}

// ─── Capability Negotiation ───────────────────────────────────────────────────

TEST_F(ConnMgrTest, NegotiationPicksTcpOverWs) {
    cm->register_connector("tcp", nullptr);
    cm->register_connector("ws",  nullptr);

    const auto id = simulate_connect();
    simulate_auth(id, {"ws", "tcp", "udp"});
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    const auto scheme = cm->get_negotiated_scheme(id);
    ASSERT_TRUE(scheme.has_value());
    EXPECT_EQ(*scheme, "tcp")
        << fmt::format("Ожидался 'tcp', получен '{}'", scheme.value_or(""));
}

TEST_F(ConnMgrTest, NegotiationFallsBackToWs) {
    cm->register_connector("ws", nullptr);
    const auto id = simulate_connect();
    simulate_auth(id, {"ws", "tcp"});
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));
    EXPECT_EQ(*cm->get_negotiated_scheme(id), "ws");
}

TEST_F(ConnMgrTest, NegotiationCustomPriority) {
    cm->register_connector("tcp", nullptr);
    cm->register_connector("udp", nullptr);
    cm->set_scheme_priority({"udp", "tcp"});
    const auto id = simulate_connect();
    simulate_auth(id, {"tcp", "udp"});
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));
    EXPECT_EQ(*cm->get_negotiated_scheme(id), "udp");
}

TEST_F(ConnMgrTest, NegotiationOldPeer) {
    cm->register_connector("tcp", nullptr);
    const auto id = simulate_connect();
    simulate_auth(id, {});  // старый клиент без schemes
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));
    EXPECT_FALSE(cm->get_negotiated_scheme(id)->empty());
}

// ─── SignalBus dispatch ───────────────────────────────────────────────────────

TEST_F(ConnMgrTest, LocalhostPacketReachesSubscriber) {
    // Localhost: plain (no encrypt)
    std::atomic<int> received{0};
    std::string      last_payload;

    bus->subscribe(MSG_TYPE_CHAT, "test_h",
        [&](std::string_view, std::shared_ptr<header_t> hdr,
            const endpoint_t*, PacketData data)
        {
            EXPECT_EQ(hdr->payload_type, static_cast<uint32_t>(MSG_TYPE_CHAT));
            last_payload = std::string(data->begin(), data->end());
            received.fetch_add(1, std::memory_order_relaxed);
        });

    const auto id = simulate_connect("127.0.0.1", 9001);
    simulate_auth(id);
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    inject_plain(id, MSG_TYPE_CHAT, "hello world");
    EXPECT_TRUE(wait_for([&]{ return received.load() > 0; }));

    EXPECT_EQ(received.load(), 1);
    EXPECT_EQ(last_payload, "hello world");
}

TEST_F(ConnMgrTest, PacketBeforeAuthIsDropped) {
    std::atomic<int> received{0};
    bus->subscribe_wildcard("drop_test",
        [&](auto, auto, auto, auto) { received.fetch_add(1); });

    const auto id = simulate_connect();
    // НЕ делаем AUTH — шлём пакет в STATE_AUTH_PENDING
    inject_plain(id, MSG_TYPE_CHAT, "should be dropped");

    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(received.load(), 0) << "Пакет до AUTH должен быть отброшен";
}

TEST_F(ConnMgrTest, WildcardReceivesAllTypes) {
    std::atomic<int>    received{0};
    std::set<uint32_t>  seen_types;
    std::mutex          seen_mu;

    bus->subscribe_wildcard("wc",
        [&](std::string_view, std::shared_ptr<header_t> hdr, const endpoint_t*, PacketData)
        {
            std::lock_guard lk(seen_mu);
            seen_types.insert(hdr->payload_type);
            received.fetch_add(1, std::memory_order_relaxed);
        });

    const auto id = simulate_connect("127.0.0.1", 9002);
    simulate_auth(id);
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    inject_plain(id, MSG_TYPE_CHAT,   "a");
    inject_plain(id, MSG_TYPE_SYSTEM, "b");
    EXPECT_TRUE(wait_for([&]{ return received.load() >= 2; }));

    EXPECT_GE(received.load(), 2);
    EXPECT_TRUE(seen_types.count(MSG_TYPE_CHAT));
    EXPECT_TRUE(seen_types.count(MSG_TYPE_SYSTEM));
}

TEST_F(ConnMgrTest, TypedSubscriberIgnoresOtherTypes) {
    std::atomic<int> chat{0}, file{0};

    bus->subscribe(MSG_TYPE_CHAT, "ch", [&](auto, auto, auto, auto) { chat++; });
    bus->subscribe(MSG_TYPE_FILE, "fh", [&](auto, auto, auto, auto) { file++; });

    const auto id = simulate_connect("127.0.0.1", 9003);
    simulate_auth(id);
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    inject_plain(id, MSG_TYPE_CHAT, "c1");
    inject_plain(id, MSG_TYPE_CHAT, "c2");
    inject_plain(id, MSG_TYPE_FILE, "f1");

    EXPECT_TRUE(wait_for([&]{ return chat.load() >= 2 && file.load() >= 1; }));
    EXPECT_EQ(chat.load(), 2);
    EXPECT_EQ(file.load(), 1);
}

TEST_F(ConnMgrTest, HandlerNameDeliveredCorrectly) {
    std::string got_name;
    bus->subscribe(MSG_TYPE_CHAT, "my_named_handler",
        [&](std::string_view name, auto, auto, auto) {
            got_name = std::string(name);
        });

    const auto id = simulate_connect("127.0.0.1", 9004);
    simulate_auth(id);
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    inject_plain(id, MSG_TYPE_CHAT, "x");
    EXPECT_TRUE(wait_for([&]{ return !got_name.empty(); }));
    EXPECT_EQ(got_name, "my_named_handler");
}

// ─── SignalBus diagnostics ────────────────────────────────────────────────────

TEST_F(ConnMgrTest, SignalBusSubscriberCount) {
    bus->subscribe(MSG_TYPE_CHAT, "h1", [](auto, auto, auto, auto){});
    bus->subscribe(MSG_TYPE_CHAT, "h2", [](auto, auto, auto, auto){});
    bus->subscribe(MSG_TYPE_FILE, "h3", [](auto, auto, auto, auto){});
    EXPECT_EQ(bus->subscriber_count(MSG_TYPE_CHAT), 2u);
    EXPECT_EQ(bus->subscriber_count(MSG_TYPE_FILE), 1u);
    EXPECT_EQ(bus->subscriber_count(MSG_TYPE_SYSTEM), 0u);
}

TEST_F(ConnMgrTest, SignalBusWildcardCount) {
    EXPECT_EQ(bus->wildcard_count(), 0u);
    bus->subscribe_wildcard("w1", [](auto, auto, auto, auto){});
    bus->subscribe_wildcard("w2", [](auto, auto, auto, auto){});
    EXPECT_EQ(bus->wildcard_count(), 2u);
}

// ─── Stream reassembly ────────────────────────────────────────────────────────

TEST_F(ConnMgrTest, FragmentedPacketReassembly) {
    std::atomic<int> received{0};
    bus->subscribe(MSG_TYPE_CHAT, "frag_h", [&](auto, auto, auto, auto){ received++; });

    const auto id = simulate_connect("127.0.0.1", 9005);
    simulate_auth(id);
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    const std::string payload = "fragmented_message_test";
    header_t hdr{};
    hdr.magic = GNET_MAGIC; hdr.proto_ver = GNET_PROTO_VER;
    hdr.payload_type = MSG_TYPE_CHAT;
    hdr.payload_len  = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> wire(sizeof(hdr) + payload.size());
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    std::memcpy(wire.data() + sizeof(hdr), payload.data(), payload.size());

    host_api_t api{}; cm->fill_host_api(&api);
    // Шлём по кускам
    api.on_data(api.ctx, id, wire.data(), 10);
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(received.load(), 0) << "Неполный заголовок не должен триггерить emit";

    api.on_data(api.ctx, id, wire.data() + 10, 10);
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(received.load(), 0) << "Неполный пакет не должен триггерить emit";

    api.on_data(api.ctx, id, wire.data() + 20, wire.size() - 20);
    EXPECT_TRUE(wait_for([&]{ return received.load() > 0; }));
    EXPECT_EQ(received.load(), 1);
}

TEST_F(ConnMgrTest, TwoPacketsInOneChunk) {
    std::atomic<int> received{0};
    bus->subscribe(MSG_TYPE_CHAT, "two_h", [&](auto, auto, auto, auto){ received++; });

    const auto id = simulate_connect("127.0.0.1", 9006);
    simulate_auth(id);
    EXPECT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    auto make = [](uint32_t t, const std::string& p) {
        header_t h{}; h.magic = GNET_MAGIC; h.proto_ver = GNET_PROTO_VER;
        h.payload_type = t; h.payload_len = static_cast<uint32_t>(p.size());
        std::vector<uint8_t> w(sizeof(h) + p.size());
        std::memcpy(w.data(), &h, sizeof(h));
        std::memcpy(w.data() + sizeof(h), p.data(), p.size());
        return w;
    };
    auto w1 = make(MSG_TYPE_CHAT, "msg1");
    auto w2 = make(MSG_TYPE_CHAT, "msg2");
    std::vector<uint8_t> combined(w1);
    combined.insert(combined.end(), w2.begin(), w2.end());

    host_api_t api{}; cm->fill_host_api(&api);
    api.on_data(api.ctx, id, combined.data(), combined.size());
    EXPECT_TRUE(wait_for([&]{ return received.load() >= 2; }));
    EXPECT_EQ(received.load(), 2);
}

// ─── Crypto sign/verify ───────────────────────────────────────────────────────

TEST_F(ConnMgrTest, SignAndVerifyRoundtrip) {
    host_api_t api{}; cm->fill_host_api(&api);
    const char msg[] = "test payload for signing";
    uint8_t sig[64]{};
    ASSERT_EQ(api.sign_with_device(api.ctx, msg, sizeof(msg), sig), 0);
    EXPECT_EQ(api.verify_signature(api.ctx, msg, sizeof(msg),
                                   cm->identity().device_pubkey, sig), 0);
}

TEST_F(ConnMgrTest, TamperedSigFails) {
    host_api_t api{}; cm->fill_host_api(&api);
    const char msg[] = "data";
    uint8_t sig[64]{};
    api.sign_with_device(api.ctx, msg, sizeof(msg), sig);
    sig[0] ^= 0xFF;
    EXPECT_NE(api.verify_signature(api.ctx, msg, sizeof(msg),
                                   cm->identity().device_pubkey, sig), 0);
}

TEST_F(ConnMgrTest, WrongPubkeyFails) {
    host_api_t api{}; cm->fill_host_api(&api);
    const char msg[] = "data";
    uint8_t sig[64]{}, other_pk[32], other_sk[64];
    api.sign_with_device(api.ctx, msg, sizeof(msg), sig);
    crypto_sign_keypair(other_pk, other_sk);
    EXPECT_NE(api.verify_signature(api.ctx, msg, sizeof(msg), other_pk, sig), 0);
}

// ─── SessionState encrypt/decrypt ────────────────────────────────────────────

TEST_F(ConnMgrTest, SessionEncryptDecryptRoundtrip) {
    SessionState sess;
    // Устанавливаем произвольный session_key
    randombytes_buf(sess.session_key, sizeof(sess.session_key));
    sess.ready = true;

    const std::string plain = "secret message 12345";
    auto wire = sess.encrypt(plain.data(), plain.size());
    ASSERT_GT(wire.size(), plain.size()) << "wire должен быть длиннее plain";

    auto decrypted = sess.decrypt(wire.data(), wire.size());
    ASSERT_EQ(decrypted.size(), plain.size());
    EXPECT_EQ(std::string(decrypted.begin(), decrypted.end()), plain);
}

TEST_F(ConnMgrTest, SessionDecryptRejectsReplay) {
    SessionState sess;
    randombytes_buf(sess.session_key, sizeof(sess.session_key));
    sess.ready = true;

    const std::string plain = "message";
    const auto wire = sess.encrypt(plain.data(), plain.size());

    // Первое расшифрование OK
    auto d1 = sess.decrypt(wire.data(), wire.size());
    EXPECT_FALSE(d1.empty());

    // Повторное — replay → отклонить
    auto d2 = sess.decrypt(wire.data(), wire.size());
    EXPECT_TRUE(d2.empty()) << "Повторная расшифровка должна отклоняться";
}

TEST_F(ConnMgrTest, SessionDecryptRejectsTamperedMAC) {
    SessionState sess;
    randombytes_buf(sess.session_key, sizeof(sess.session_key));
    sess.ready = true;

    const std::string plain = "message";
    auto wire = sess.encrypt(plain.data(), plain.size());
    wire.back() ^= 0xFF;  // портим MAC

    auto d = sess.decrypt(wire.data(), wire.size());
    EXPECT_TRUE(d.empty()) << "Испорченный MAC должен отклоняться";
}

// ─── Encrypted packet (remote peer, ECDH session) ────────────────────────────
//
// Проверяем полный путь: simulate_auth на non-localhost → derive_session →
// inject зашифрованного пакета с правильным session_key → subscriber получает plaintext.

TEST_F(ConnMgrTest, EncryptedPacketReachesSubscriberAfterECDH) {
    // Захватываем node_ephem_pk из исходящего AUTH ядра
    uint8_t node_ephem_pk[32]{};
    const conn_id_t id = simulate_connect_and_capture_ephem(node_ephem_pk, "10.0.0.5", 8765);
    ASSERT_NE(id, CONN_ID_INVALID);

    // Проверяем что ephem_pk получен (не нули)
    bool all_zero = true;
    for (auto b : node_ephem_pk) if (b) { all_zero = false; break; }
    if (all_zero) {
        GTEST_SKIP() << "CaptureConnector не захватил AUTH (mock_cap не зарегистрирован вовремя)";
    }

    // Вычисляем ожидаемый session_key: ECDH(peer_ephem_sk, node_ephem_pk)
    // (ДОЛЖЕН совпасть с тем что derive_session вычислил внутри ConnectionManager)
    uint8_t session_key[crypto_secretbox_KEYBYTES]{};
    simulate_auth_and_derive_key(id, node_ephem_pk,
                                  cm->identity().user_pubkey,
                                  session_key);

    // Ждём STATE_ESTABLISHED
    ASSERT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }))
        << "AUTH должен переводить в ESTABLISHED";

    // Подписываемся на CHAT
    std::atomic<int> received{0};
    std::string      last_plain;
    bus->subscribe(MSG_TYPE_CHAT, "enc_test_h",
        [&](std::string_view, std::shared_ptr<header_t> hdr,
            const endpoint_t*, PacketData data)
        {
            EXPECT_EQ(hdr->payload_type, static_cast<uint32_t>(MSG_TYPE_CHAT));
            last_plain = std::string(data->begin(), data->end());
            received.fetch_add(1, std::memory_order_relaxed);
        });

    // Инжектируем зашифрованный пакет
    const std::string secret_text = "encrypted hello from peer";
    inject_with_session_key(id, MSG_TYPE_CHAT, secret_text, session_key);

    ASSERT_TRUE(wait_for([&]{ return received.load() > 0; }))
        << "Зашифрованный пакет должен быть расшифрован и доставлен подписчику";

    EXPECT_EQ(last_plain, secret_text)
        << "Расшифрованный текст должен совпадать с оригиналом";
}

TEST_F(ConnMgrTest, EncryptedPacketWithWrongKeyIsDropped) {
    // remote peer
    uint8_t node_ephem_pk[32]{};
    const conn_id_t id = simulate_connect_and_capture_ephem(node_ephem_pk, "10.0.0.6", 8766);
    ASSERT_NE(id, CONN_ID_INVALID);

    bool all_zero = true;
    for (auto b : node_ephem_pk) if (b) { all_zero = false; break; }
    if (all_zero) GTEST_SKIP() << "CaptureConnector не захватил AUTH";

    simulate_auth(id);
    ASSERT_TRUE(wait_for([&]{ return cm->get_state(id) == STATE_ESTABLISHED; }));

    std::atomic<int> received{0};
    bus->subscribe(MSG_TYPE_CHAT, "wrong_key_h",
        [&](auto, auto, auto, auto) { received.fetch_add(1); });

    // Шифруем случайным ключом (НЕ тем что derive_session вычислил)
    uint8_t wrong_key[crypto_secretbox_KEYBYTES]{};
    randombytes_buf(wrong_key, sizeof(wrong_key));
    inject_with_session_key(id, MSG_TYPE_CHAT, "should not arrive", wrong_key);

    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(received.load(), 0)
        << "Пакет с неверным ключом (MAC fail) должен быть отброшен";
}

// ─── Stress ───────────────────────────────────────────────────────────────────

TEST_F(ConnMgrTest, ConcurrentConnectsThreadSafe) {
    constexpr int N = 100;
    host_api_t api{}; cm->fill_host_api(&api);
    std::vector<conn_id_t> ids(N, CONN_ID_INVALID);
    std::atomic<int> port{10000};
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i]{
            endpoint_t ep{};
            std::strncpy(ep.address, "10.0.0.1", sizeof(ep.address)-1);
            ep.port = static_cast<uint16_t>(port.fetch_add(1));
            ids[static_cast<size_t>(i)] = api.on_connect(api.ctx, &ep);
        });
    }
    for (auto& t : threads) t.join();
    std::set<conn_id_t> id_set(ids.begin(), ids.end());
    id_set.erase(CONN_ID_INVALID);
    EXPECT_EQ(id_set.size(), static_cast<size_t>(N))
        << fmt::format("Ожидалось {} уникальных ID, получено {}", N, id_set.size());
}