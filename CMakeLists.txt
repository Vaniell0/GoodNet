cmake_minimum_required(VERSION 3.22.1)
project(GoodNet)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Сначала определяем опции
option(BUILD_PLUGINS "Build plugins with the project" ON)
option(USE_UPX "Compress binary with UPX" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Настройки оптимизации
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -flto -ffunction-sections -fdata-sections -march=native)
    add_link_options(-Wl,--gc-sections -flto -s)
    add_compile_definitions(NDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    add_compile_options(-Os -ffunction-sections -fdata-sections -fomit-frame-pointer)
    add_link_options(-Wl,--gc-sections -s -Wl,--strip-all)
    add_compile_definitions(NDEBUG SIZE_OPTIMIZED)
endif()

# Поиск библиотек
cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(sodium_PKG QUIET libsodium)
endif()

# Подключаем плагины, ТОЛЬКО если включена опция
if(BUILD_PLUGINS)
    add_subdirectory(plugins/handlers)
    add_subdirectory(plugins/connectors)
endif()

# Исполняемый файл
add_executable(goodnet
    src/main.cpp
    src/core.cpp
    
    src/config.cpp
    src/logger.cpp
    src/signals.cpp
    
    core/homeServices.cpp
    core/pluginManager.cpp
    core/connectManager.cpp
)

target_include_directories(goodnet PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/sdk
)

target_link_libraries(goodnet PRIVATE
    Boost::filesystem
    spdlog::spdlog
    Boost::system
    sodium dl
    fmt::fmt
)

# UPX
if(USE_UPX)
    find_program(UPX_PROGRAM upx)
    if(UPX_PROGRAM)
        add_custom_command(TARGET goodnet POST_BUILD
            COMMAND ${UPX_PROGRAM} --best --lzma $<TARGET_FILE:goodnet>
            COMMENT "Compressing with UPX"
        )
    endif()
endif()

# Создание ссылок для разработки
# Этот шаг нужен, чтобы при локальной разработке плагины были видны в build/plugins
set(PLUGINS_LINK_DIR "${CMAKE_BINARY_DIR}/plugins")
add_custom_target(setup_plugin_links ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PLUGINS_LINK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGINS_LINK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/plugins/handlers/libs" "${PLUGINS_LINK_DIR}/handlers"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/plugins/connectors/libs" "${PLUGINS_LINK_DIR}/connectors"
    COMMENT "Linking plugin libs to build directory"
)
add_dependencies(goodnet setup_plugin_links)

# Установка (для Nix build)
install(TARGETS goodnet DESTINATION bin)
install(DIRECTORY include/sdk DESTINATION include/goodnet)