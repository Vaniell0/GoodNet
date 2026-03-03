# cmake/pch.cmake
#
# Precompiled Headers — ускоряют инкрементальную сборку в nix develop.
# В Nix-деривациях (BUILD_TESTING=ON, чистая сборка) PCH не нужен:
# каждый TU компилируется один раз, кеш деривации хранит весь вывод.
#
# Отключается переменной GOODNET_DISABLE_PCH (автоматически при Nix):
#   cmake -DGOODNET_DISABLE_PCH=ON ...
#
# Включить явно для nix develop:
#   cfgd  (алиас передаёт без этого флага → PCH активен)

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