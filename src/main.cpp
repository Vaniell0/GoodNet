#include "config.hpp"
#include "logger.hpp"
#include "pluginManager.hpp"

int main(int argc, char* argv[]) {
    try {        
        Config config;

        Logger::initialize(
            config.get_or<std::string>("logging.level", "info"),
            config.get_or<std::string>("logging.file", "logs/goodnet.log"),
            config.get_or<int>("logging.max_size", 10 * 1024 * 1024),
            config.get_or<int>("logging.max_files", 5)
        );
        
        LOG_INFO("GoodNet starting...");
        LOG_INFO("Listen address: {}", config.get_or<std::string>("core.listen_address", "0.0.0.0"));
        LOG_INFO("Listen port: {}", config.get_or<int>("core.listen_port", 25565));
        
        host_api_t api {
            .api_version = 1,
            .send = nullptr,
            .create_connection = nullptr,
            .close_connection = nullptr,
            .update_connection_state = nullptr
        };

        LOG_INFO("Created host API with version: {}", api.api_version);
        
        gn::PluginManager pm(&api, config.get_or<std::string>("plugins.base_dir", "./plugins"));
        
        pm.load_all_plugins();
        
        pm.list_plugins();

        Logger::shutdown();
        return 0;

    } catch (const std::exception& e) {
        LOG_CRITICAL("Fatal error: {}", e.what());
        return 1;
    }
}