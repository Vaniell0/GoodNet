#pragma once
/// @file apps/store/store_app.hpp
/// @brief Store orchestrator: gn::Core + IStore + StoreHandler.

#include "backend.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class Config;
namespace gn { class Core; }

namespace gn::store {

class StoreHandler;

/// @brief Конфигурация Store.
struct StoreConfig {
    std::string listen_address  = "0.0.0.0";
    uint16_t    listen_port     = 25566;
    std::string backend_type    = "sqlite";
    std::string db_path         = "store.db";
    uint32_t    cleanup_interval_s = 600;   ///< 10 мин
    std::vector<std::string> seed_peers;    ///< "tcp://host:port"
};

/// @brief Минимальный шаблон Store-приложения.
///
/// Создаёт gn::Core, backend, handler. Подключается к seed peers.
/// Периодически чистит просроченные записи.
class StoreApp {
public:
    explicit StoreApp(const StoreConfig& cfg);
    ~StoreApp();

    StoreApp(const StoreApp&) = delete;
    StoreApp& operator=(const StoreApp&) = delete;

    /// Запустить (блокирующий — ждёт stop() из другого потока или сигнала).
    void run();

    /// Остановить.
    void stop();

private:
    StoreConfig cfg_;

    std::unique_ptr<Config>         gn_config_;
    std::unique_ptr<gn::Core>       core_;
    std::unique_ptr<IStore>  backend_;
    std::unique_ptr<StoreHandler>   handler_;

    std::atomic<bool> running_{false};
    std::thread       cleanup_thread_;

    void setup_connection_tracking();
    void start_cleanup_loop();
};

} // namespace gn::store
