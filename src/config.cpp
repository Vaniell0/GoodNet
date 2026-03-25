/// @file src/config.cpp

#include "config.hpp"
#include "logger.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

// ── Construction ──────────────────────────────────────────────────────────────

Config::Config(bool defaults_only) {
    if (!defaults_only)
        load_from_file(fs::current_path() / "config.json");
}

// ── Persistence ───────────────────────────────────────────────────────────────

bool Config::load_from_file(const fs::path& path) {
    if (!fs::exists(path)) {
        LOG_WARN("config not found: {}, using defaults", path.string());
        return false;
    }
    try {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("cannot open: {}", path.string()); return false; }
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!parse_json(ss.str())) return false;
        last_file_ = path;
        LOG_INFO("config loaded: {}", path.string());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("config load error: {}", e.what());
        return false;
    }
}

bool Config::load_from_string(const std::string& json) {
    return parse_json(json);
}

bool Config::reload() {
    if (last_file_.empty()) return false;
    return load_from_file(last_file_);
}

bool Config::save_to_file(const fs::path& path) const {
    try {
        fs::create_directories(path.parent_path());
        std::ofstream f(path);
        if (!f) { LOG_ERROR("cannot write: {}", path.string()); return false; }
        f << to_json();
        LOG_INFO("config saved: {}", path.string());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("config save error: {}", e.what());
        return false;
    }
}

std::string Config::save_to_string() const { return to_json(); }

// ── JSON parsing ──────────────────────────────────────────────────────────────

bool Config::parse_json(const std::string& json_str) {
    try {
        const auto j = nlohmann::json::parse(json_str);

        if (j.contains("core")) {
            const auto& c = j["core"];
            if (c.contains("listen_address") && c["listen_address"].is_string())
                core.listen_address = c["listen_address"];
            if (c.contains("listen_port") && c["listen_port"].is_number_integer())
                core.listen_port = c["listen_port"];
            if (c.contains("io_threads") && c["io_threads"].is_number_integer())
                core.io_threads = c["io_threads"];
            if (c.contains("max_connections") && c["max_connections"].is_number_integer())
                core.max_connections = c["max_connections"];
        }

        if (j.contains("logging")) {
            const auto& l = j["logging"];
            if (l.contains("level") && l["level"].is_string())
                logging.level = l["level"];
            if (l.contains("file") && l["file"].is_string())
                logging.file = l["file"];
            if (l.contains("max_size") && l["max_size"].is_number_integer())
                logging.max_size = l["max_size"];
            if (l.contains("max_files") && l["max_files"].is_number_integer())
                logging.max_files = l["max_files"];
        }

        if (j.contains("security")) {
            const auto& s = j["security"];
            if (s.contains("key_exchange_timeout") && s["key_exchange_timeout"].is_number_integer())
                security.key_exchange_timeout = s["key_exchange_timeout"];
            if (s.contains("max_auth_attempts") && s["max_auth_attempts"].is_number_integer())
                security.max_auth_attempts = s["max_auth_attempts"];
            if (s.contains("session_timeout") && s["session_timeout"].is_number_integer())
                security.session_timeout = s["session_timeout"];
        }

        if (j.contains("compression")) {
            const auto& c = j["compression"];
            if (c.contains("enabled") && c["enabled"].is_boolean())
                compression.enabled = c["enabled"];
            if (c.contains("threshold") && c["threshold"].is_number_integer())
                compression.threshold = c["threshold"];
            if (c.contains("level") && c["level"].is_number_integer())
                compression.level = c["level"];
        }

        if (j.contains("plugins")) {
            const auto& p = j["plugins"];
            if (p.contains("base_dir") && p["base_dir"].is_string())
                plugins.base_dir = p["base_dir"];
            if (p.contains("auto_load") && p["auto_load"].is_boolean())
                plugins.auto_load = p["auto_load"];
            if (p.contains("scan_interval") && p["scan_interval"].is_number_integer())
                plugins.scan_interval = p["scan_interval"];
            if (p.contains("extra_dirs") && p["extra_dirs"].is_string())
                plugins.extra_dirs = p["extra_dirs"];
        }

        if (j.contains("identity")) {
            const auto& i = j["identity"];
            if (i.contains("dir") && i["dir"].is_string())
                identity.dir = i["dir"];
            if (i.contains("ssh_key_path") && i["ssh_key_path"].is_string())
                identity.ssh_key_path = i["ssh_key_path"];
            if (i.contains("use_machine_id") && i["use_machine_id"].is_boolean())
                identity.use_machine_id = i["use_machine_id"];
            if (i.contains("skip_ssh_fallback") && i["skip_ssh_fallback"].is_boolean())
                identity.skip_ssh_fallback = i["skip_ssh_fallback"];
        }

        if (j.contains("ice")) {
            const auto& ic = j["ice"];
            if (ic.contains("stun_servers") && ic["stun_servers"].is_string())
                ice.stun_servers = ic["stun_servers"];
            if (ic.contains("session_timeout") && ic["session_timeout"].is_number_integer())
                ice.session_timeout = ic["session_timeout"];
            if (ic.contains("keepalive_interval") && ic["keepalive_interval"].is_number_integer())
                ice.keepalive_interval = ic["keepalive_interval"];
            if (ic.contains("consent_max_failures") && ic["consent_max_failures"].is_number_integer())
                ice.consent_max_failures = ic["consent_max_failures"];
        }

        return true;
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("JSON parse error: {}", e.what());
        return false;
    }
}

std::string Config::to_json() const {
    nlohmann::json j;

    j["core"] = {
        {"listen_address", core.listen_address},
        {"listen_port",    core.listen_port},
        {"io_threads",     core.io_threads},
        {"max_connections", core.max_connections},
    };

    j["logging"] = {
        {"level",     logging.level},
        {"file",      logging.file},
        {"max_size",  logging.max_size},
        {"max_files", logging.max_files},
    };

    j["security"] = {
        {"key_exchange_timeout", security.key_exchange_timeout},
        {"max_auth_attempts",    security.max_auth_attempts},
        {"session_timeout",      security.session_timeout},
    };

    j["compression"] = {
        {"enabled",   compression.enabled},
        {"threshold", compression.threshold},
        {"level",     compression.level},
    };

    j["plugins"] = {
        {"base_dir",      plugins.base_dir},
        {"auto_load",     plugins.auto_load},
        {"scan_interval", plugins.scan_interval},
        {"extra_dirs",    plugins.extra_dirs},
    };

    j["identity"] = {
        {"dir",               identity.dir},
        {"ssh_key_path",      identity.ssh_key_path},
        {"use_machine_id",    identity.use_machine_id},
        {"skip_ssh_fallback", identity.skip_ssh_fallback},
    };

    j["ice"] = {
        {"stun_servers",       ice.stun_servers},
        {"session_timeout",    ice.session_timeout},
        {"keepalive_interval", ice.keepalive_interval},
        {"consent_max_failures", ice.consent_max_failures},
    };

    return j.dump(2);
}

// ── Flat key access ──────────────────────────────────────────────────────────

// Хелпер: извлечь первый host:port из CSV строки
static std::pair<std::string, std::string> extract_first_stun(const std::string& csv) {
    auto comma = csv.find(',');
    std::string first = (comma != std::string::npos) ? csv.substr(0, comma) : csv;
    // Trim
    while (!first.empty() && first.front() == ' ') first.erase(0, 1);
    while (!first.empty() && first.back() == ' ')  first.pop_back();

    if (auto colon = first.rfind(':'); colon != std::string::npos)
        return {first.substr(0, colon), first.substr(colon + 1)};
    return {first, "19302"};
}

std::optional<std::string> Config::get_raw(std::string_view key) const {
    // Core
    if (key == "core.listen_address")  return core.listen_address;
    if (key == "core.listen_port")     return std::to_string(core.listen_port);
    if (key == "core.io_threads")      return std::to_string(core.io_threads);
    if (key == "core.max_connections") return std::to_string(core.max_connections);
    // Logging
    if (key == "logging.level")     return logging.level;
    if (key == "logging.file")      return logging.file;
    if (key == "logging.max_size")  return std::to_string(logging.max_size);
    if (key == "logging.max_files") return std::to_string(logging.max_files);
    // Security
    if (key == "security.key_exchange_timeout") return std::to_string(security.key_exchange_timeout);
    if (key == "security.max_auth_attempts")    return std::to_string(security.max_auth_attempts);
    if (key == "security.session_timeout")      return std::to_string(security.session_timeout);
    // Compression
    if (key == "compression.enabled")   return std::string(compression.enabled ? "true" : "false");
    if (key == "compression.threshold") return std::to_string(compression.threshold);
    if (key == "compression.level")     return std::to_string(compression.level);
    // Plugins
    if (key == "plugins.base_dir")      return plugins.base_dir;
    if (key == "plugins.auto_load")     return std::string(plugins.auto_load ? "true" : "false");
    if (key == "plugins.scan_interval") return std::to_string(plugins.scan_interval);
    if (key == "plugins.extra_dirs")    return plugins.extra_dirs;
    // Identity
    if (key == "identity.dir")              return identity.dir;
    if (key == "identity.ssh_key_path")     return identity.ssh_key_path;
    if (key == "identity.use_machine_id")   return std::string(identity.use_machine_id ? "true" : "false");
    if (key == "identity.skip_ssh_fallback") return std::string(identity.skip_ssh_fallback ? "true" : "false");
    // Ice
    if (key == "ice.stun_servers")       return ice.stun_servers;
    if (key == "ice.session_timeout")    return std::to_string(ice.session_timeout);
    if (key == "ice.keepalive_interval") return std::to_string(ice.keepalive_interval);
    if (key == "ice.consent_max_failures" || key == "ice.consent_failures")
        return std::to_string(ice.consent_max_failures);
    // Legacy single-server aliases (extract first entry from CSV)
    if (key == "ice.stun_server") return extract_first_stun(ice.stun_servers).first;
    if (key == "ice.stun_port")   return extract_first_stun(ice.stun_servers).second;

    return std::nullopt;
}
