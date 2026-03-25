# helper.cmake — утилита для сборки плагинов GoodNet
# Устанавливается в lib/cmake/GoodNet/, подключается через:
#   find_package(GoodNet REQUIRED)
#   include(${GOODNET_SDK_HELPER})   ← автоматически после find_package

message(STATUS "🔧 Plugin Builder | SDK: ${GOODNET_SDK_PATH}")
message(STATUS "🔧 Plugin Builder | Inc: ${GOODNET_INC_PATH}")

# Загружаем pch.cmake если он установлен (для apply_plugin_pch)
if(DEFINED GOODNET_PCH_CMAKE AND EXISTS "${GOODNET_PCH_CMAKE}")
    include("${GOODNET_PCH_CMAKE}")
endif()

# ── add_plugin ────────────────────────────────────────────────────────────────
#
# Создаёт SHARED таргет плагина с правильными настройками.
# Вызывается из CMakeLists.txt плагина:
#
#   add_plugin(myhandler myhandler.cpp extra.cpp)
#
# Особенности:
#   • PREFIX "lib" → libmyhandler.so (ожидаемый формат PluginManager)
#   • CXX_VISIBILITY_PRESET hidden → не загрязняем глобальное символьное пространство
#   • НЕ линкует GoodNet::core напрямую — символы приходят через RTLD_GLOBAL от ядра.
#     Линковать GoodNet::core в плагин = дублировать Logger и другие static → UB.
#   • PCH для Debug: apply_plugin_pch ускоряет инкрементальные пересборки в nix develop.

macro(add_plugin NAME)
    add_library(${NAME} SHARED ${ARGN})

    set_target_properties(${NAME} PROPERTIES
        PREFIX   "lib"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    # Заголовки SDK и ядра
    target_include_directories(${NAME} PRIVATE
        ${GOODNET_INC_PATH}
        ${GOODNET_SDK_PATH}
        ${GOODNET_SDK_PATH}/cpp
    )

    # Оптимизация размера .so
    target_compile_options(${NAME} PRIVATE -Os -ffunction-sections -fdata-sections)
    if(NOT APPLE)
        target_link_options(${NAME} PRIVATE -Wl,--gc-sections)
    endif()

    # PCH для Debug (no-op в Release)
    if(COMMAND apply_plugin_pch)
        apply_plugin_pch(${NAME})
    endif()
endmacro()
