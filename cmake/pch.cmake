# cmake/pch.cmake
#
# Precompiled Headers — speed up incremental builds in nix develop.
# In Nix derivations (clean builds) PCH is unnecessary: each TU compiles
# once and the derivation cache stores all output.
#
# Disabled by GOODNET_DISABLE_PCH (set automatically in Nix derivations):
#   cmake -DGOODNET_DISABLE_PCH=ON ...
#
# Enabled implicitly in Debug builds within nix develop:
#   cfgd  (alias passes Debug without the disable flag -> PCH active)

option(GOODNET_DISABLE_PCH "Disable precompiled headers (set ON in CI/Nix builds)" OFF)

set(GOODNET_PCH_SYSTEM
    <string> <string_view> <vector> <array>
    <unordered_map> <unordered_set> <map> <set>
    <optional> <variant> <expected>
    <memory> <filesystem> <functional>
    <algorithm> <numeric> <ranges> <span>
    <chrono> <atomic> <mutex> <shared_mutex> <thread>
    <concepts> <type_traits> <stdexcept> <cassert>
    <fmt/core.h> <fmt/format.h> <fmt/chrono.h>
    <spdlog/spdlog.h>
    <spdlog/sinks/rotating_file_sink.h>
    <spdlog/sinks/stdout_color_sinks.h>
    <nlohmann/json.hpp>
)

function(apply_pch TARGET)
    if(GOODNET_DISABLE_PCH OR NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()
    target_precompile_headers(${TARGET} PRIVATE
        ${GOODNET_PCH_SYSTEM}
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/include/logger.hpp>"
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/include/config.hpp>"
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/include/dynlib.hpp>"
    )
    message(STATUS "[PCH] Enabled for target '${TARGET}' (Debug)")
endfunction()

set(GOODNET_PCH_PLUGIN
    <string> <string_view> <vector> <memory>
    <functional> <filesystem> <span> <optional>
    <fmt/core.h> <fmt/format.h>
    <spdlog/spdlog.h>
)

function(apply_plugin_pch TARGET)
    if(GOODNET_DISABLE_PCH OR NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()
    target_precompile_headers(${TARGET} PRIVATE ${GOODNET_PCH_PLUGIN})
    message(STATUS "[PCH] Plugin PCH enabled for '${TARGET}' (Debug)")
endfunction()
