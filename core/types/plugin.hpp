#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "../sdk/connector.h"
#include "../sdk/handler.h"
#include "dynlib.hpp"

namespace gn {

namespace fs = std::filesystem;

// ── HandlerInfo ───────────────────────────────────────────────────────────────

/// Loaded handler plugin — owns the DynLib and the handler_t instance.
struct HandlerInfo {
    DynLib      lib;
    handler_t*  handler = nullptr;
    host_api_t  api{};
    fs::path    path;
    std::string name;
    std::atomic<bool> enabled{true};

    ~HandlerInfo();
    HandlerInfo()                              = default;
    HandlerInfo(const HandlerInfo&)            = delete;
    HandlerInfo& operator=(const HandlerInfo&) = delete;
    HandlerInfo(HandlerInfo&&) noexcept;
    HandlerInfo& operator=(HandlerInfo&&) noexcept;
};

// ── ConnectorInfo ─────────────────────────────────────────────────────────────

/// Loaded connector plugin — owns the DynLib and the connector_ops_t instance.
struct ConnectorInfo {
    DynLib           lib;
    connector_ops_t* ops    = nullptr;
    host_api_t       api{};
    fs::path         path;
    std::string      name;
    std::string      scheme;
    std::atomic<bool> enabled{true};

    ConnectorInfo()                                = default;
    ~ConnectorInfo();
    ConnectorInfo(const ConnectorInfo&)            = delete;
    ConnectorInfo& operator=(const ConnectorInfo&) = delete;
    ConnectorInfo(ConnectorInfo&&) noexcept;
    ConnectorInfo& operator=(ConnectorInfo&&) noexcept;
};

} // namespace gn
