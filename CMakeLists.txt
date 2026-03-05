cmake_minimum_required(VERSION 3.22)
project(GoodNet)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

cmake_policy(SET CMP0167 NEW)

option(INSTALL_DEVELOPMENT "Install SDK headers and cmake config" OFF)
option(BUILD_TESTING "Build the testing suite" OFF)

# ─── Зависимости ─────────────────────────────────────────────────────────────

find_package(Boost         REQUIRED COMPONENTS system filesystem)
find_package(nlohmann_json REQUIRED)
find_package(spdlog        REQUIRED)
find_package(fmt           REQUIRED)
find_package(GTest QUIET)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

# ─── PCH ─────────────────────────────────────────────────────────────────────
include(cmake/pch.cmake)

# ─── 1. SDK (INTERFACE) ───────────────────────────────────────────────────────

add_library(goodnet_sdk INTERFACE)
add_library(GoodNet::sdk ALIAS goodnet_sdk)
set_target_properties(goodnet_sdk PROPERTIES EXPORT_NAME sdk)

target_include_directories(goodnet_sdk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk/cpp>
    $<INSTALL_INTERFACE:include/goodnet/sdk>
    $<INSTALL_INTERFACE:include/goodnet/sdk/cpp>
)

# ─── 2. Core (SHARED) ────────────────────────────────────────────────────────
#
# SHARED обязателен: одна копия статических переменных (Logger, once_flag).
# Плагин через dlopen(RTLD_GLOBAL) видит тот же Logger что и ядро.

add_library(goodnet_core SHARED
    src/config.cpp
    src/logger.cpp
    core/pluginManager_core.cpp
    core/pluginManager_state.cpp
    core/pluginManager_lookup.cpp
    core/cm_send.cpp
    core/cm_session.cpp
    core/cm_identity.cpp
    core/cm_dispatch.cpp
    core/cm_handshake.cpp
)

add_library(GoodNet::core ALIAS goodnet_core)
set_target_properties(goodnet_core PROPERTIES
    EXPORT_NAME core
    VERSION     0.1.0
    SOVERSION   0
    ENABLE_EXPORTS ON      # -rdynamic: плагины видят символы ядра
)

target_link_libraries(goodnet_core PUBLIC goodnet_sdk)

target_include_directories(goodnet_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core>
        $<INSTALL_INTERFACE:include/goodnet>
    PRIVATE
        ${SODIUM_INCLUDE_DIRS}
)

target_link_libraries(goodnet_core
    PUBLIC  spdlog::spdlog fmt::fmt
    PRIVATE nlohmann_json::nlohmann_json
            Boost::system
            ${SODIUM_LIBRARIES}
)

target_compile_definitions(goodnet_core
    PUBLIC  SPDLOG_COMPILED_LIB
    PRIVATE GOODNET_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/"
)

apply_pch(goodnet_core)

# ─── 3. Executable ────────────────────────────────────────────────────────────

add_executable(goodnet src/main.cpp)
target_link_libraries(goodnet PRIVATE goodnet_core)
target_compile_definitions(goodnet PRIVATE
    GOODNET_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/"
)
apply_pch(goodnet)

# ─── 4. Установка ────────────────────────────────────────────────────────────

if(INSTALL_DEVELOPMENT)
    install(DIRECTORY sdk/      DESTINATION include/goodnet/sdk)
    install(DIRECTORY sdk/cpp/  DESTINATION include/goodnet/sdk/cpp)
    install(DIRECTORY include/  DESTINATION include/goodnet)
    install(DIRECTORY core/     DESTINATION include/goodnet/core
            FILES_MATCHING PATTERN "*.hpp")

    install(FILES cmake/pch.cmake      DESTINATION lib/cmake/GoodNet)
    install(FILES plugins/helper.cmake DESTINATION lib/cmake/GoodNet)

    install(TARGETS goodnet_core goodnet_sdk
            EXPORT  GoodNetTargets
            ARCHIVE DESTINATION lib
            LIBRARY DESTINATION lib
            INCLUDES DESTINATION include/goodnet)

    install(EXPORT GoodNetTargets
            FILE      GoodNetTargets.cmake
            NAMESPACE GoodNet::
            DESTINATION lib/cmake/GoodNet)

    configure_file(cmake/GoodNetConfig.cmake.in
                   "${CMAKE_BINARY_DIR}/GoodNetConfig.cmake" @ONLY)
    install(FILES "${CMAKE_BINARY_DIR}/GoodNetConfig.cmake"
            DESTINATION lib/cmake/GoodNet)
endif()

install(TARGETS goodnet      RUNTIME DESTINATION bin)
install(TARGETS goodnet_core LIBRARY DESTINATION lib)

# ─── 5. Тесты ────────────────────────────────────────────────────────────────

if(BUILD_TESTING AND GTest_FOUND)
    enable_testing()

    add_library(mock_handler SHARED tests/mock_handler.cpp)
    target_link_libraries(mock_handler PRIVATE goodnet_core)

    add_library(mock_connector SHARED tests/mock_connector.cpp)
    target_link_libraries(mock_connector PRIVATE goodnet_core)

    add_executable(unit_tests
        tests/conf.cpp
        tests/plugins.cpp
        tests/connection_manager.cpp
    )

    set_target_properties(unit_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    target_compile_definitions(unit_tests PRIVATE
        MOCK_HANDLER_PATH="$<TARGET_FILE:mock_handler>"
        MOCK_CONNECTOR_PATH="$<TARGET_FILE:mock_connector>"
    )

    target_link_libraries(unit_tests PRIVATE
        goodnet_core
        GTest::gmock_main
        ${SODIUM_LIBRARIES}
    )

    target_include_directories(unit_tests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${SODIUM_INCLUDE_DIRS}
    )

    add_test(NAME AllTests COMMAND unit_tests)
endif()
