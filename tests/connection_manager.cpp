#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <filesystem>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

#include <boost/asio.hpp>
#include <sodium.h>

#include "core/connectionManager.hpp"
#include "include/signals.hpp"
#include "../sdk/types.h"
#include "../sdk/plugin.h"
#include "../sdk/connector.h"

using namespace gn;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ─── Фикстура ────────────────────────────────────────────────────────────────
//
// Для тестов ConnectionManager нужны:
//   1. Временная директория для ключей (изолированная от ~/.goodnet)
//   2. io_context + PacketSignal
//   3. NodeIdentity с известными ключами

class ConnMgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (sodium_init() < 0)
            GTEST_SKIP() << "libsodium init failed";

        key_dir = fs::temp_directory_path() /
                  fmt::format("goodnet_cm_test_{}", ::getpid());
        fs::create_directories(key_dir);

        ioc   = std::make_unique<boost::asio::io_context>();
        work  = std::make_unique<boost::asio::executor_work_guard<
                    boost::asio::io_context::executor_type>>(
                    boost::asio::make_work_guard(*ioc));
        sig   = std::make_unique<gn::PacketSignal>(*ioc);

        io_thread = std::thread([this] { ioc->run(); });

        auto identity = gn::NodeIdentity::load_or_generate(key_dir);
        cm = std::make_unique<gn::ConnectionManager>(*sig, std::move(identity));
    }

    void TearDown() override {
        work->reset();
        ioc->stop();
        if (io_thread.joinable()) io_thread.join();
        fs::remove_all(key_dir);
    }

    // Симулировать входящее соединение через on_connect коллбэк
    conn_id_t simulate_connect(const char* addr = "127.0.0.1",
                               uint16_t    port = 9000) {
        host_api_t api{};
        cm->fill_host_api(&api);

        endpoint_t ep{};
        std::strncpy(ep.address, addr, sizeof(ep.address) - 1);
        ep.port = port;

        return api.on_connect(api.ctx, &ep);
    }

    // Симулировать поступление пакета (после STATE_ESTABLISHED)
    void simulate_data(conn_id_t id,
                       uint32_t  msg_type,
                       const std::string& payload_text) {
        host_api_t api{};
        cm->fill_host_api(&api);

        // Собираем wire-пакет: header_t + payload
        header_t hdr{};
        hdr.magic        = GNET_MAGIC;
        hdr.proto_ver    = GNET_PROTO_VER;
        hdr.payload_type = msg_type;
        hdr.payload_len  = static_cast<uint32_t>(payload_text.size());

        std::vector<uint8_t> wire(sizeof(hdr) + payload_text.size());
        std::memcpy(wire.data(), &hdr, sizeof(hdr));
        std::memcpy(wire.data() + sizeof(hdr),
                    payload_text.data(), payload_text.size());

        api.on_data(api.ctx, id, wire.data(), wire.size());
    }

    // Симулировать AUTH-обмен с реальными ключами (второй узел)
    void simulate_auth_from_peer(conn_id_t id) {
        // Генерируем keypair "пира"
        uint8_t peer_user_pk[32], peer_user_sk[64];
        uint8_t peer_dev_pk[32],  peer_dev_sk[64];
        crypto_sign_keypair(peer_user_pk, peer_user_sk);
        crypto_sign_keypair(peer_dev_pk,  peer_dev_sk);

        // Подписываем как при AUTH
        uint8_t to_sign[64];
        std::memcpy(to_sign,      peer_user_pk, 32);
        std::memcpy(to_sign + 32, peer_dev_pk,  32);

        auth_payload_t ap{};
        std::memcpy(ap.user_pubkey,   peer_user_pk, 32);
        std::memcpy(ap.device_pubkey, peer_dev_pk,  32);
        crypto_sign_detached(ap.signature, nullptr,
                             to_sign, sizeof(to_sign),
                             peer_user_sk);

        // Оборачиваем в header_t
        header_t hdr{};
        hdr.magic        = GNET_MAGIC;
        hdr.proto_ver    = GNET_PROTO_VER;
        hdr.payload_type = MSG_TYPE_AUTH;
        hdr.payload_len  = sizeof(ap);

        std::vector<uint8_t> wire(sizeof(hdr) + sizeof(ap));
        std::memcpy(wire.data(), &hdr, sizeof(hdr));
        std::memcpy(wire.data() + sizeof(hdr), &ap, sizeof(ap));

        host_api_t api{};
        cm->fill_host_api(&api);
        api.on_data(api.ctx, id, wire.data(), wire.size());

        // Сохраняем peer_user_pk для проверок
        std::memcpy(last_peer_user_pk, peer_user_pk, 32);
    }

    uint8_t last_peer_user_pk[32]{};

    fs::path  key_dir;
    std::unique_ptr<boost::asio::io_context>               ioc;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>            work;
    std::unique_ptr<gn::PacketSignal>                       sig;
    std::unique_ptr<gn::ConnectionManager>                  cm;
    std::thread                                             io_thread;
};

// ─── NodeIdentity ─────────────────────────────────────────────────────────────

TEST_F(ConnMgrTest, IdentityGeneratesOnFirstRun) {
    // Ключи должны быть сгенерированы и файлы созданы
    EXPECT_TRUE(fs::exists(key_dir / "user_key"));
    EXPECT_TRUE(fs::exists(key_dir / "device_key"));
}

TEST_F(ConnMgrTest, IdentityPersistsAcrossReloads) {
    // Первый pubkey
    std::string pubkey_1 = cm->identity().user_pubkey_hex();

    // Перезагружаем идентификатор из тех же файлов
    auto identity2 = gn::NodeIdentity::load_or_generate(key_dir);
    std::string pubkey_2 = identity2.user_pubkey_hex();

    EXPECT_EQ(pubkey_1, pubkey_2) << "Keys must not regenerate on reload";
}

TEST_F(ConnMgrTest, IdentityPubkeyIsValidHex) {
    std::string hex = cm->identity().user_pubkey_hex();
    ASSERT_EQ(hex.size(), 64u); // 32 bytes * 2 hex chars
    for (char c : hex)
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)))
            << "Non-hex char: " << c;
}

TEST_F(ConnMgrTest, UserAndDevicePubkeysAreDifferent) {
    EXPECT_NE(cm->identity().user_pubkey_hex(),
              cm->identity().device_pubkey_hex());
}

// ─── ConnectionRecord: on_connect ────────────────────────────────────────────

TEST_F(ConnMgrTest, OnConnectReturnsValidId) {
    conn_id_t id = simulate_connect();
    EXPECT_NE(id, CONN_ID_INVALID);
}

TEST_F(ConnMgrTest, OnConnectIncrementsCount) {
    EXPECT_EQ(cm->connection_count(), 0u);
    simulate_connect("10.0.0.1", 8001);
    EXPECT_EQ(cm->connection_count(), 1u);
    simulate_connect("10.0.0.2", 8002);
    EXPECT_EQ(cm->connection_count(), 2u);
}

TEST_F(ConnMgrTest, OnConnectIdsAreUnique) {
    conn_id_t a = simulate_connect("10.0.0.1", 8001);
    conn_id_t b = simulate_connect("10.0.0.2", 8002);
    conn_id_t c = simulate_connect("10.0.0.3", 8003);

    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
}

TEST_F(ConnMgrTest, OnConnectInitialStateIsAuthPending) {
    conn_id_t id = simulate_connect();
    auto state = cm->get_state(id);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_AUTH_PENDING);
}

// ─── AUTH flow ────────────────────────────────────────────────────────────────

TEST_F(ConnMgrTest, ValidAuthTransitionsToEstablished) {
    conn_id_t id = simulate_connect();
    EXPECT_EQ(*cm->get_state(id), STATE_AUTH_PENDING);

    simulate_auth_from_peer(id);

    // После корректного AUTH → STATE_ESTABLISHED
    auto state = cm->get_state(id);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(*state, STATE_ESTABLISHED);
}

TEST_F(ConnMgrTest, BadMagicDropsBuffer) {
    conn_id_t id = simulate_connect();
    simulate_auth_from_peer(id);

    // Отправляем мусор с неверным magic
    uint8_t garbage[64] = {};
    garbage[0] = 0xDE; garbage[1] = 0xAD; // не GNET_MAGIC
    host_api_t api{}; cm->fill_host_api(&api);
    api.on_data(api.ctx, id, garbage, sizeof(garbage));

    // Состояние не должно упасть в сегфолт — просто продолжаем
    EXPECT_EQ(*cm->get_state(id), STATE_ESTABLISHED);
}

TEST_F(ConnMgrTest, TamperedAuthSignatureRejected) {
    conn_id_t id = simulate_connect();

    // Строим AUTH с подписью, потом портим её
    uint8_t pk[32], sk[64];
    crypto_sign_keypair(pk, sk);

    uint8_t dev_pk[32], dev_sk[64];
    crypto_sign_keypair(dev_pk, dev_sk);

    uint8_t to_sign[64];
    std::memcpy(to_sign,      pk,     32);
    std::memcpy(to_sign + 32, dev_pk, 32);

    auth_payload_t ap{};
    std::memcpy(ap.user_pubkey,   pk,     32);
    std::memcpy(ap.device_pubkey, dev_pk, 32);
    crypto_sign_detached(ap.signature, nullptr, to_sign, sizeof(to_sign), sk);
    ap.signature[0] ^= 0xFF; // портим байт

    header_t hdr{};
    hdr.magic        = GNET_MAGIC;
    hdr.proto_ver    = GNET_PROTO_VER;
    hdr.payload_type = MSG_TYPE_AUTH;
    hdr.payload_len  = sizeof(ap);

    std::vector<uint8_t> wire(sizeof(hdr) + sizeof(ap));
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    std::memcpy(wire.data() + sizeof(hdr), &ap, sizeof(ap));

    host_api_t api{}; cm->fill_host_api(&api);
    api.on_data(api.ctx, id, wire.data(), wire.size());

    // Должны остаться в AUTH_PENDING
    auto state = cm->get_state(id);
    ASSERT_TRUE(state.has_value());
    EXPECT_NE(*state, STATE_ESTABLISHED)
        << "Tampered AUTH must not establish session";
}

// ─── PacketSignal: маршрутизация пакетов ─────────────────────────────────────

TEST_F(ConnMgrTest, EstablishedPacketReachesSignal) {
    std::atomic<int> received{0};
    uint32_t         last_type = 0;
    std::string      last_payload;

    sig->connect([&](std::shared_ptr<header_t> hdr,
                     const endpoint_t*,
                     gn::PacketData data) {
        last_type    = hdr->payload_type;
        last_payload = std::string(data->begin(), data->end());
        received.fetch_add(1, std::memory_order_relaxed);
    });

    conn_id_t id = simulate_connect();
    simulate_auth_from_peer(id);           // → STATE_ESTABLISHED
    simulate_data(id, MSG_TYPE_CHAT, "hello"); // → emit PacketSignal

    // Ждём обработки в io_context (max 500ms)
    for (int i = 0; i < 50 && received.load() == 0; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(received.load(), 1);
    EXPECT_EQ(last_type, static_cast<uint32_t>(MSG_TYPE_CHAT));
    EXPECT_EQ(last_payload, "hello");
}

TEST_F(ConnMgrTest, PacketBeforeAuthIsDropped) {
    std::atomic<int> received{0};
    sig->connect([&](auto, auto, auto) { received.fetch_add(1); });

    conn_id_t id = simulate_connect();
    // НЕ делаем AUTH — шлём пакет в STATE_AUTH_PENDING
    simulate_data(id, MSG_TYPE_CHAT, "should be dropped");

    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(received.load(), 0) << "Packet before AUTH must be dropped";
}

TEST_F(ConnMgrTest, TcpFragmentationReassembly) {
    std::atomic<int> received{0};
    sig->connect([&](auto, auto, auto) { received.fetch_add(1); });

    conn_id_t id = simulate_connect();
    simulate_auth_from_peer(id);

    host_api_t api{}; cm->fill_host_api(&api);

    // Собираем целый пакет, потом режем его на 3 части
    std::string payload = "fragmented_message";
    header_t hdr{};
    hdr.magic        = GNET_MAGIC;
    hdr.proto_ver    = GNET_PROTO_VER;
    hdr.payload_type = MSG_TYPE_CHAT;
    hdr.payload_len  = static_cast<uint32_t>(payload.size());

    std::vector<uint8_t> wire(sizeof(hdr) + payload.size());
    std::memcpy(wire.data(), &hdr, sizeof(hdr));
    std::memcpy(wire.data() + sizeof(hdr), payload.data(), payload.size());

    // Часть 1: первые 10 байт
    api.on_data(api.ctx, id, wire.data(), 10);
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(received.load(), 0) << "Should not emit on partial header";

    // Часть 2: ещё 10 байт
    api.on_data(api.ctx, id, wire.data() + 10, 10);
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(received.load(), 0) << "Should not emit on partial packet";

    // Часть 3: остаток
    api.on_data(api.ctx, id, wire.data() + 20, wire.size() - 20);

    for (int i = 0; i < 50 && received.load() == 0; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(received.load(), 1) << "Must reassemble from fragments";
}

TEST_F(ConnMgrTest, MultiplePacketsInOneChunk) {
    std::atomic<int> received{0};
    sig->connect([&](auto, auto, auto) { received.fetch_add(1); });

    conn_id_t id = simulate_connect();
    simulate_auth_from_peer(id);

    // Клеим два пакета в один буфер
    auto make_wire = [](uint32_t type, const std::string& pl) {
        header_t h{};
        h.magic        = GNET_MAGIC;
        h.proto_ver    = GNET_PROTO_VER;
        h.payload_type = type;
        h.payload_len  = static_cast<uint32_t>(pl.size());
        std::vector<uint8_t> w(sizeof(h) + pl.size());
        std::memcpy(w.data(), &h, sizeof(h));
        std::memcpy(w.data() + sizeof(h), pl.data(), pl.size());
        return w;
    };

    auto w1 = make_wire(MSG_TYPE_CHAT, "msg1");
    auto w2 = make_wire(MSG_TYPE_CHAT, "msg2");
    std::vector<uint8_t> combined(w1);
    combined.insert(combined.end(), w2.begin(), w2.end());

    host_api_t api{}; cm->fill_host_api(&api);
    api.on_data(api.ctx, id, combined.data(), combined.size());

    for (int i = 0; i < 50 && received.load() < 2; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(received.load(), 2) << "Both packets must be dispatched";
}

// ─── on_disconnect ────────────────────────────────────────────────────────────

TEST_F(ConnMgrTest, OnDisconnectRemovesRecord) {
    conn_id_t id = simulate_connect();
    EXPECT_EQ(cm->connection_count(), 1u);

    host_api_t api{}; cm->fill_host_api(&api);
    api.on_disconnect(api.ctx, id, 0);

    EXPECT_EQ(cm->connection_count(), 0u);
    EXPECT_FALSE(cm->get_state(id).has_value());
}

TEST_F(ConnMgrTest, OnDisconnectUnknownIdIsNoOp) {
    // Не должен крашнуться на несуществующем conn_id
    host_api_t api{}; cm->fill_host_api(&api);
    EXPECT_NO_THROW(api.on_disconnect(api.ctx, 999999, 0));
}

// ─── Crypto: sign / verify ────────────────────────────────────────────────────

TEST_F(ConnMgrTest, SignAndVerifyRoundtrip) {
    host_api_t api{}; cm->fill_host_api(&api);

    const char msg[] = "test signing payload";
    uint8_t sig_bytes[64]{};

    int ret = api.sign_with_device(api.ctx, msg, sizeof(msg), sig_bytes);
    ASSERT_EQ(ret, 0);

    // Верифицируем device pubkey'ом этого узла
    int ok = api.verify_signature(
        api.ctx,
        msg, sizeof(msg),
        cm->identity().device_pubkey,
        sig_bytes);
    EXPECT_EQ(ok, 0) << "Signature must verify with device pubkey";
}

TEST_F(ConnMgrTest, TamperedSignatureFailsVerify) {
    host_api_t api{}; cm->fill_host_api(&api);

    const char msg[] = "payload";
    uint8_t    sig_bytes[64]{};
    api.sign_with_device(api.ctx, msg, sizeof(msg), sig_bytes);

    sig_bytes[0] ^= 0xFF; // портим
    int ret = api.verify_signature(
        api.ctx, msg, sizeof(msg),
        cm->identity().device_pubkey, sig_bytes);
    EXPECT_NE(ret, 0) << "Tampered signature must fail";
}

TEST_F(ConnMgrTest, WrongKeyFailsVerify) {
    host_api_t api{}; cm->fill_host_api(&api);

    const char msg[] = "payload";
    uint8_t    sig_bytes[64]{};
    api.sign_with_device(api.ctx, msg, sizeof(msg), sig_bytes);

    // Другой pubkey
    uint8_t other_pk[32], other_sk[64];
    crypto_sign_keypair(other_pk, other_sk);
    int ret = api.verify_signature(
        api.ctx, msg, sizeof(msg), other_pk, sig_bytes);
    EXPECT_NE(ret, 0) << "Signature must fail with wrong pubkey";
}

// ─── Stress: конкурентные соединения ─────────────────────────────────────────

TEST_F(ConnMgrTest, ConcurrentConnectsAreThreadSafe) {
    constexpr int N = 100;
    host_api_t api{}; cm->fill_host_api(&api);

    std::vector<std::thread> threads;
    std::vector<conn_id_t>   ids(static_cast<size_t>(N), CONN_ID_INVALID);
    std::atomic<int>         port{10000};

    threads.reserve(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i] {
            endpoint_t ep{};
            std::strncpy(ep.address, "10.0.0.1", sizeof(ep.address) - 1);
            ep.port = static_cast<uint16_t>(port.fetch_add(1));
            ids[static_cast<size_t>(i)] = api.on_connect(api.ctx, &ep);
        });
    }
    for (auto& t : threads) t.join();

    // Все conn_id уникальны и валидны
    std::set<conn_id_t> id_set(ids.begin(), ids.end());
    id_set.erase(CONN_ID_INVALID);
    EXPECT_EQ(id_set.size(), static_cast<size_t>(N));
    EXPECT_EQ(cm->connection_count(), static_cast<size_t>(N));
}

TEST_F(ConnMgrTest, ConcurrentDataCallsAreThreadSafe) {
    conn_id_t id = simulate_connect();
    simulate_auth_from_peer(id);
    
    // Ждем, пока AUTH обработается и состояние обновится
    std::this_thread::sleep_for(10ms); 
    ASSERT_EQ(*cm->get_state(id), STATE_ESTABLISHED);

    std::atomic<int> received{0}; // Создаем счетчик ПОСЛЕ авторизации
    sig->connect([&](auto, auto, auto) { 
        received.fetch_add(1, std::memory_order_relaxed); 
    });

    host_api_t api{}; cm->fill_host_api(&api);
    constexpr int N = 50;

    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&] {
            std::string text = "concurrent";
            header_t h{};
            h.magic = GNET_MAGIC; h.proto_ver = GNET_PROTO_VER;
            h.payload_type = MSG_TYPE_CHAT;
            h.payload_len = static_cast<uint32_t>(text.size());
            std::vector<uint8_t> w(sizeof(h) + text.size());
            std::memcpy(w.data(), &h, sizeof(h));
            std::memcpy(w.data() + sizeof(h), text.data(), text.size());
            api.on_data(api.ctx, id, w.data(), w.size());
        });
    }
    for (auto& t : threads) t.join();

    for (int i = 0; i < 100 && received.load() < N; ++i)
        std::this_thread::sleep_for(10ms);

    EXPECT_EQ(received.load(), N);
}
