/// @file src/config.cpp

#include "config.hpp"
#include "logger.hpp"

#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <sstream>

// ── Static defaults ───────────────────────────────────────────────────────────

const std::string Config::Core::LISTEN_ADDRESS   = "0.0.0.0";
const unsigned    Config::Core::LISTEN_PORT       = 25565;
const short       Config::Core::IO_THREADS        = 4;
const size_t      Config::Core::MAX_CONNECTIONS   = 1000;

const std::string Config::Logging::LEVEL          = "info";
const std::string Config::Logging::FILE           = "logs/goodnet.log";
const int         Config::Logging::MAX_SIZE        = 10 * 1024 * 1024;
const int         Config::Logging::MAX_FILES       = 5;

const fs::path    Config::Plugins::BASE_DIR        = fs::current_path() / "plugins";
const bool        Config::Plugins::AUTO_LOAD       = true;
const int         Config::Plugins::SCAN_INTERVAL   = 300;

const int         Config::Security::KEY_EXCHANGE_TIMEOUT = 30;
const int         Config::Security::MAX_AUTH_ATTEMPTS    = 3;
const int         Config::Security::SESSION_TIMEOUT      = 3600;

// ── Construction ──────────────────────────────────────────────────────────────

Config::Config(bool defaults_only) {
    setup_defaults();
    if (!defaults_only)
        load_from_file(fs::current_path() / "config.json");
}

void Config::setup_defaults() {
    set("core.listen_address",  Core::LISTEN_ADDRESS);
    set("core.listen_port",     static_cast<int>(Core::LISTEN_PORT));
    set("core.io_threads",      static_cast<int>(Core::IO_THREADS));
    set("core.max_connections", static_cast<int>(Core::MAX_CONNECTIONS));

    set("logging.level",     Logging::LEVEL);
    set("logging.file",      Logging::FILE);
    set("logging.max_size",  Logging::MAX_SIZE);
    set("logging.max_files", Logging::MAX_FILES);

    set("plugins.base_dir",      Plugins::BASE_DIR);
    set("plugins.auto_load",     Plugins::AUTO_LOAD);
    set("plugins.scan_interval", Plugins::SCAN_INTERVAL);

    set("security.key_exchange_timeout", Security::KEY_EXCHANGE_TIMEOUT);
    set("security.max_auth_attempts",    Security::MAX_AUTH_ATTEMPTS);
    set("security.session_timeout",      Security::SESSION_TIMEOUT);
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

// ── Mutation ──────────────────────────────────────────────────────────────────

void Config::remove(const std::string& key) {
    if (values_.erase(key)) {
        LOG_DEBUG("config removed: {}", key);
        notify(key);
    }
}

bool Config::has(const std::string& key) const {
    return values_.contains(key);
}

std::vector<std::string> Config::keys_in(std::string_view section) const {
    std::vector<std::string> out;
    std::string prefix = std::string(section) + '.';
    for (auto& [k, _] : values_)
        if (k.starts_with(prefix)) out.push_back(k);
    return out;
}

// ── Change notifications ──────────────────────────────────────────────────────

uint64_t Config::watch(const std::string& key, WatchCallback cb) {
    uint64_t id = next_watch_id_++;
    watchers_[id] = {key, std::move(cb)};
    return id;
}

void Config::unwatch(uint64_t id) {
    watchers_.erase(id);
}

void Config::notify(const std::string& key) {
    auto it = values_.find(key);
    if (it == values_.end()) return;
    for (auto& [_, w] : watchers_)
        if (w.key == key) w.cb(key, it->second);
}

// ── JSON parsing ──────────────────────────────────────────────────────────────

bool Config::parse_json(const std::string& json_str) {
    try {
        nlohmann::json j = nlohmann::json::parse(json_str);

        std::function<void(const std::string&, const nlohmann::json&)> walk;
        walk = [&](const std::string& prefix, const nlohmann::json& obj) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                std::string k = prefix.empty() ? it.key() : prefix + '.' + it.key();
                if      (it->is_object())          walk(k, *it);
                else if (it->is_string())           set(k, it->get<std::string>());
                else if (it->is_number_integer())   set(k, it->get<int>());
                else if (it->is_number_float())     set(k, it->get<double>());
                else if (it->is_boolean())          set(k, it->get<bool>());
            }
        };
        walk("", j);
        return true;
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("JSON parse error: {}", e.what());
        return false;
    }
}

std::string Config::to_json() const {
    nlohmann::json j;
    for (auto& [key, value] : values_) {
        std::vector<std::string> parts;
        std::istringstream ss(key);
        for (std::string p; std::getline(ss, p, '.'); ) parts.push_back(p);

        nlohmann::json* cur = &j;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            if (!cur->contains(parts[i])) (*cur)[parts[i]] = nlohmann::json::object();
            cur = &(*cur)[parts[i]];
        }
        std::visit([&](const auto& v) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, fs::path>)
                (*cur)[parts.back()] = v.string();
            else
                (*cur)[parts.back()] = v;
        }, value);
    }
    return j.dump(2);
}

// ── get<T> specialisations ────────────────────────────────────────────────────

template<typename T>
std::optional<T> Config::get(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;

    if constexpr (std::is_same_v<T, fs::path>) {
        if (const auto* p = std::get_if<fs::path>  (&it->second)) return *p;
        if (const auto* s = std::get_if<std::string>(&it->second)) return fs::path(*s);
        LOG_ERROR("config type mismatch: '{}'", key);
        return std::nullopt;
    } else {
        try { return std::get<T>(it->second); }
        catch (const std::bad_variant_access&) {
            LOG_ERROR("config type mismatch: '{}'", key);
            return std::nullopt;
        }
    }
}

// ── Explicit instantiations ───────────────────────────────────────────────────

template void Config::set<int>        (const std::string&, const int&);
template void Config::set<bool>       (const std::string&, const bool&);
template void Config::set<double>     (const std::string&, const double&);
template void Config::set<std::string>(const std::string&, const std::string&);
template void Config::set<fs::path>   (const std::string&, const fs::path&);

template std::optional<int>         Config::get<int>        (const std::string&) const;
template std::optional<bool>        Config::get<bool>       (const std::string&) const;
template std::optional<double>      Config::get<double>     (const std::string&) const;
template std::optional<std::string> Config::get<std::string>(const std::string&) const;
template std::optional<fs::path>    Config::get<fs::path>   (const std::string&) const;

template int         Config::get_or<int>        (const std::string&, const int&)         const;
template bool        Config::get_or<bool>       (const std::string&, const bool&)        const;
template double      Config::get_or<double>     (const std::string&, const double&)      const;
template std::string Config::get_or<std::string>(const std::string&, const std::string&) const;
template fs::path    Config::get_or<fs::path>   (const std::string&, const fs::path&)    const;