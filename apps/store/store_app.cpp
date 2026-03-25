/// @file apps/store/store_app.cpp
/// @brief Store orchestrator implementation.

#include "store_app.hpp"
#include "handler.hpp"
#include "sqlite_backend.hpp"

#include "core.hpp"
#include "config.hpp"
#include "signals.hpp"

#include <iostream>
#include <chrono>

namespace gn::store {

StoreApp::StoreApp(const StoreConfig& cfg) : cfg_(cfg) {
    // Конфиг ядра
    gn_config_ = std::make_unique<Config>(true);
    gn_config_->core.listen_port = cfg_.listen_port;
    gn_config_->core.listen_address = cfg_.listen_address;
    gn_config_->plugins.auto_load = false;  // Store не нуждается в плагинах

    // Ядро
    core_ = std::make_unique<gn::Core>(gn_config_.get());

    // Backend
    if (cfg_.backend_type == "sqlite") {
        backend_ = std::make_unique<Sqlite>(cfg_.db_path);
    } else {
        throw std::runtime_error("Unknown backend type: " + cfg_.backend_type);
    }

    // Handler
    handler_ = std::make_unique<StoreHandler>(*core_, *backend_);
}

StoreApp::~StoreApp() {
    stop();
}

void StoreApp::run() {
    running_.store(true, std::memory_order_relaxed);

    // Запустить ядро
    core_->run_async();

    // Подписать handler на Store-сообщения
    handler_->start();

    // Трекинг соединений (для очистки подписок)
    setup_connection_tracking();

    // Подключиться к seed peers
    for (const auto& peer : cfg_.seed_peers) {
        std::cerr << "[store] Connecting to seed: " << peer << "\n";
        core_->connect(peer);
    }

    // Cleanup loop в фоне
    start_cleanup_loop();

    std::cerr << "[store] Running on " << cfg_.listen_address
              << ":" << cfg_.listen_port << "\n";

    // Блокирующий цикл
    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void StoreApp::stop() {
    if (!running_.exchange(false, std::memory_order_relaxed))
        return;

    std::cerr << "[store] Shutting down...\n";

    handler_->stop();

    if (cleanup_thread_.joinable())
        cleanup_thread_.join();

    core_->stop();
}

void StoreApp::setup_connection_tracking() {
    core_->bus().on_conn_state.connect(
        [this](conn_id_t id, conn_state_t state) {
            if (state == STATE_CLOSED || state == STATE_CLOSING) {
                handler_->on_disconnect(id);
            }
        });
}

void StoreApp::start_cleanup_loop() {
    cleanup_thread_ = std::thread([this]() {
        while (running_.load(std::memory_order_relaxed)) {
            auto interval = std::chrono::seconds(cfg_.cleanup_interval_s);
            auto deadline = std::chrono::steady_clock::now() + interval;

            while (running_.load(std::memory_order_relaxed) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!running_.load(std::memory_order_relaxed)) break;

            uint64_t removed = backend_->cleanup_expired();
            if (removed > 0) {
                std::cerr << "[store] Cleaned up " << removed << " expired entries\n";
            }
        }
    });
}

} // namespace gn::store
