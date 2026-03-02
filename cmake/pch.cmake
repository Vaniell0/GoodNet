# cmake/pch.cmake
#
# Precompiled Headers для Debug-сборок.
# Подключается из главного CMakeLists.txt:
#
#   include(cmake/pch.cmake)
#   # ... определяем таргеты ...
#   apply_pch(goodnet_core)
#   apply_pch(goodnet)
#
# В Release PCH не нужен: Nix кеширует весь вывод деривации целиком.
# Инкрементальная пересборка происходит только в nix develop + cmake.
# PCH там ускоряет повторные компиляции в 2-5x для тяжёлых заголовков.
#
# Архитектура:
#   SYSTEM headers (spdlog, fmt, nlohmann, STL) — меняются крайне редко.
#     Компилируются в .gch один раз и переиспользуются пока не изменятся.
#   PROJECT headers (logger.hpp, config.hpp) — меняются чаще.
#     Включены в PCH для удобства, перестраиваются при изменении.

# Системные / внешние заголовки
# Порядок важен: включай более базовые раньше
set(GOODNET_PCH_SYSTEM
    # ── C++ STL ───────────────────────────────────────────────────
    <string>
    <string_view>
    <vector>
    <array>
    <unordered_map>
    <unordered_set>
    <map>
    <set>
    <optional>
    <variant>
    <expected>
    <memory>
    <filesystem>
    <functional>
    <algorithm>
    <numeric>
    <ranges>
    <span>
    <chrono>
    <atomic>
    <mutex>
    <shared_mutex>
    <thread>
    <concepts>
    <type_traits>
    <stdexcept>
    <cassert>
    # ── fmt / spdlog — самые тяжёлые (много шаблонного кода) ─────
    <fmt/core.h>
    <fmt/format.h>
    <spdlog/spdlog.h>
    <spdlog/sinks/rotating_file_sink.h>
    <spdlog/sinks/stdout_color_sinks.h>
    # ── nlohmann/json — тяжелее всего, ~30k строк шаблонов ───────
    <nlohmann/json.hpp>
)

# Применяет PCH к cmake-таргету.
# Только в Debug — функция no-op в Release/RelWithDebInfo/MinSizeRel.
function(apply_pch TARGET)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()

    target_precompile_headers(${TARGET}
        PRIVATE
            # Внешние заголовки (в угловых скобках)
            ${GOODNET_PCH_SYSTEM}
            # Внутренние заголовки (абсолютный путь → cmake отслеживает изменения)
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/include/logger.hpp>"
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/include/config.hpp>"
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/include/dynlib.hpp>"
    )

    message(STATUS "[PCH] Enabled for target '${TARGET}' (Debug)")
endfunction()

# Для плагинов: лёгкий набор без project headers ядра.
# Вызывается из plugins/helper.cmake после add_plugin().
#
# Использование в плагине CMakeLists.txt:
#   apply_plugin_pch(logger)
set(GOODNET_PCH_PLUGIN
    <string>
    <string_view>
    <vector>
    <memory>
    <functional>
    <filesystem>
    <span>
    <optional>
    <fmt/core.h>
    <fmt/format.h>
    <spdlog/spdlog.h>
)

function(apply_plugin_pch TARGET)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()
    target_precompile_headers(${TARGET}
        PRIVATE ${GOODNET_PCH_PLUGIN}
    )
    message(STATUS "[PCH] Plugin PCH enabled for '${TARGET}' (Debug)")
endfunction()
