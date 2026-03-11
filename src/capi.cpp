#include "core.h"
#include "core.hpp"
#include <cstring>

extern "C" {

gn_core_t* gn_core_create(gn_config_t* cfg) {
    gn::CoreConfig cpp_cfg;
    if (cfg) {
        if (cfg->config_dir) cpp_cfg.identity.dir = cfg->config_dir;
        if (cfg->log_level)  cpp_cfg.logging.level = cfg->log_level;
        cpp_cfg.network.listen_port = cfg->listen_port;
    }
    // Возвращаем как непрозрачный указатель
    return reinterpret_cast<gn_core_t*>(new gn::Core(std::move(cpp_cfg)));
}

void gn_core_destroy(gn_core_t* core) {
    delete reinterpret_cast<gn::Core*>(core);
}

void gn_core_run(gn_core_t* core) {
    reinterpret_cast<gn::Core*>(core)->run();
}

void gn_core_run_async(gn_core_t* core, int threads) {
    if (!core) return;
    reinterpret_cast<gn::Core*>(core)->run_async(threads);
}

void gn_core_stop(gn_core_t* core) {
    reinterpret_cast<gn::Core*>(core)->stop();
}

void gn_core_send(gn_core_t* core, const char* uri, uint32_t type, const void* data, size_t len) {
    reinterpret_cast<gn::Core*>(core)->send(uri, type, data, len);
}

size_t gn_core_get_user_pubkey(gn_core_t* core, char* buffer, size_t max_len) {
    auto hex = reinterpret_cast<gn::Core*>(core)->user_pubkey_hex();
    size_t len = std::min(hex.size(), max_len - 1);
    std::memcpy(buffer, hex.c_str(), len);
    buffer[len] = '\0';
    return len;
}

}
