#include "pluginManager.hpp"
#include "logger.hpp"
#include <ranges>
#include <algorithm>
#include <optional>

namespace gn {

std::optional<handler_t*>
PluginManager::find_handler_by_name(std::string_view name) const {
    std::shared_lock lock(rw_mutex_);
    if (auto it = handlers_.find(name); it != handlers_.end()) {
        if (it->second->enabled) return it->second->handler;
        LOG_DEBUG("Handler '{}' found but disabled", name);
    }
    return std::nullopt;
}

std::optional<connector_ops_t*>
PluginManager::find_connector_by_scheme(std::string_view scheme) const {
    std::shared_lock lock(rw_mutex_);
    if (auto it = connectors_.find(scheme); it != connectors_.end()) {
        if (it->second->enabled) return it->second->ops;
    }
    return std::nullopt;
}

size_t PluginManager::get_enabled_handler_count() const {
    std::shared_lock lock(rw_mutex_);
    return static_cast<size_t>(
        std::ranges::count_if(handlers_ | std::views::values,
                              [](const auto& i) { return i->enabled; }));
}

size_t PluginManager::get_enabled_connector_count() const {
    std::shared_lock lock(rw_mutex_);
    return static_cast<size_t>(
        std::ranges::count_if(connectors_ | std::views::values,
                              [](const auto& i) { return i->enabled; }));
}

void PluginManager::list_plugins() const {
    std::shared_lock lock(rw_mutex_);
    LOG_INFO("=== Handlers ({}) ===", handlers_.size());
    for (const auto& [name, info] : handlers_)
        LOG_INFO("  [{}] '{}' ({})",
                 info->enabled ? "ON " : "OFF", name,
                 info->path.filename().string());
    LOG_INFO("=== Connectors ({}) ===", connectors_.size());
    for (const auto& [scheme, info] : connectors_)
        LOG_INFO("  [{}] {}:// -> '{}' ({})",
                 info->enabled ? "ON " : "OFF", scheme, info->name,
                 info->path.filename().string());
}

std::vector<handler_t*> PluginManager::get_active_handlers() const {
    std::shared_lock lock(rw_mutex_);
    std::vector<handler_t*> active;
    active.reserve(handlers_.size());
    
    for (const auto& [name, info] : handlers_) {
        if (info->enabled && info->handler) {
            active.push_back(info->handler);
        }
    }
    return active;
}

std::vector<connector_ops_t*> PluginManager::get_active_connectors() const {
    std::shared_lock lock(rw_mutex_);
    std::vector<connector_ops_t*> active;
    active.reserve(connectors_.size());
    
    for (const auto& [name, info] : connectors_) {
        if (info->enabled && info->ops) {
            active.push_back(info->ops);
        }
    }
    return active;
}

} // namespace gn
