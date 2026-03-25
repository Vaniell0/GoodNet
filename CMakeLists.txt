cmake_minimum_required(VERSION 3.22)
project(GoodNet)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    add_compile_definitions(NOMINMAX _WIN32_WINNT=0x0A00)
endif()

option(INSTALL_DEVELOPMENT "Install SDK headers and cmake config" OFF)
option(BUILD_TESTING "Build the testing suite" OFF)
option(GOODNET_STATIC_CORE "Build goodnet_core as static library" OFF)
option(GOODNET_STATIC_PLUGINS "Compile built-in plugins into the binary" OFF)

# ─── Dependencies ────────────────────────────────────────────────────────────

# CONFIG mode: use Boost's own BoostConfig.cmake (works with vcpkg and Nix).
# Avoids CMake 3.31's removed FindBoost module (CMP0167).
find_package(Boost CONFIG REQUIRED COMPONENTS program_options)
find_package(nlohmann_json REQUIRED)
find_package(spdlog        REQUIRED)
find_package(fmt           REQUIRED)
find_package(GTest QUIET)

# libsodium: try CMake config first (vcpkg), fall back to pkg-config (Linux/Nix)
find_package(unofficial-sodium CONFIG QUIET)
if(unofficial-sodium_FOUND)
    set(SODIUM_LIBRARIES unofficial-sodium::sodium)
    set(SODIUM_INCLUDE_DIRS "")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED libsodium)
endif()

# zstd: try CMake config first (vcpkg), fall back to pkg-config
find_package(zstd CONFIG QUIET)
if(zstd_FOUND)
    set(ZSTD_LIBRARIES $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)
else()
    if(NOT SODIUM_FOUND OR NOT PkgConfig_FOUND)
        find_package(PkgConfig REQUIRED)
    endif()
    pkg_check_modules(ZSTD REQUIRED libzstd)
endif()

# ─── Coverage ────────────────────────────────────────────────────────────────
option(GOODNET_COVERAGE "Enable code coverage instrumentation (gcov/lcov)" OFF)
if(GOODNET_COVERAGE)
    add_compile_options(--coverage)
    add_link_options(--coverage)
endif()

# ─── PCH ─────────────────────────────────────────────────────────────────────
include(cmake/pch.cmake)

# ─── 1. SDK (INTERFACE) ───────────────────────────────────────────────────────

add_library(goodnet_sdk INTERFACE)
add_library(GoodNet::sdk ALIAS goodnet_sdk)
set_target_properties(goodnet_sdk PROPERTIES EXPORT_NAME sdk)

target_include_directories(goodnet_sdk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk/cpp>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core/data>
    $<INSTALL_INTERFACE:include/goodnet/sdk>
    $<INSTALL_INTERFACE:include/goodnet/sdk/cpp>
    $<INSTALL_INTERFACE:include/goodnet/core/data>
)

# ─── 2. Core (SHARED) ────────────────────────────────────────────────────────
#
# SHARED is the default: single copy of static variables (Logger, once_flag).
# Plugins loaded via dlopen(RTLD_GLOBAL) see the same Logger as the core.
# STATIC is used for portable single-binary builds (e.g. Windows + GOODNET_STATIC_PLUGINS).

if(GOODNET_STATIC_CORE)
    set(_CORE_LIB_TYPE STATIC)
else()
    set(_CORE_LIB_TYPE SHARED)
endif()

add_library(goodnet_core ${_CORE_LIB_TYPE}
    src/capi.cpp
    src/core.cpp

    src/config.cpp
    src/logger.cpp
    src/signals.cpp

    core/pm/core.cpp
    core/pm/query.cpp

    core/cm/lifecycle.cpp
    core/cm/disconnect.cpp
    core/cm/queries.cpp
    core/cm/registration.cpp
    core/cm/transport.cpp
    core/cm/handshake.cpp
    core/cm/dispatch.cpp
    core/cm/relay.cpp
    core/cm/identity.cpp

    core/crypto/noise.cpp
    core/crypto/machine_id.cpp

    core/util.cpp
)

add_library(GoodNet::core ALIAS goodnet_core)
set_target_properties(goodnet_core PROPERTIES
    EXPORT_NAME core
    VERSION     0.1.0
    SOVERSION   0
    ENABLE_EXPORTS ON       # -rdynamic: plugins see core symbols
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
    PUBLIC  spdlog::spdlog
    PRIVATE nlohmann_json::nlohmann_json fmt::fmt
            ${ZSTD_LIBRARIES}
            ${SODIUM_LIBRARIES}
)

target_compile_definitions(goodnet_core
    PUBLIC  SPDLOG_COMPILED_LIB
    PRIVATE GOODNET_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/"
)

if(NOT MSVC)
    target_compile_definitions(goodnet_core PUBLIC
        $<$<CONFIG:Release>:_FORTIFY_SOURCE=2>
        $<$<CONFIG:RelWithDebInfo>:_FORTIFY_SOURCE=2>
    )
endif()

if(WIN32)
    target_link_libraries(goodnet_core PRIVATE iphlpapi advapi32 ws2_32)
endif()

apply_pch(goodnet_core)

# ─── 3. Executable ────────────────────────────────────────────────────────────

add_executable(goodnet
    src/main.cpp
    cli/bench.cpp
    cli/server.cpp
)
target_include_directories(goodnet PRIVATE ${CMAKE_SOURCE_DIR}/cli)
target_link_libraries(goodnet PRIVATE goodnet_core
    ${SODIUM_LIBRARIES}
    Boost::program_options
)
target_compile_definitions(goodnet PRIVATE
    GOODNET_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/"
)
apply_pch(goodnet)

# ─── 3b. Static plugins (optional) ──────────────────────────────────────────

if(GOODNET_STATIC_PLUGINS)
    target_sources(goodnet PRIVATE
        plugins/connectors/tcp/tcp.cpp
        plugins/handlers/logger/logger.cpp
    )
    target_compile_definitions(goodnet PRIVATE GOODNET_STATIC_PLUGINS)
    target_include_directories(goodnet PRIVATE
        ${Boost_INCLUDE_DIRS}
    )
endif()

# ─── 4. Installation ─────────────────────────────────────────────────────────

if(INSTALL_DEVELOPMENT)
    install(DIRECTORY sdk/      DESTINATION include/goodnet/sdk)
    install(DIRECTORY include/  DESTINATION include/goodnet)
    install(DIRECTORY core/     DESTINATION include/goodnet/core/
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

# ─── 3c. Store app (optional) ──────────────────────────────────────────────

option(BUILD_STORE "Build the Store distributed registry service" OFF)

if(BUILD_STORE)
    find_package(SQLite3 REQUIRED)

    add_executable(goodnet_store
        apps/store/main.cpp
        apps/store/store_app.cpp
        apps/store/sqlite_backend.cpp
        apps/store/handler.cpp
    )
    target_include_directories(goodnet_store PRIVATE
        ${CMAKE_SOURCE_DIR}/apps/store
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}
        ${SODIUM_INCLUDE_DIRS}
    )
    target_link_libraries(goodnet_store PRIVATE
        goodnet_core
        ${SODIUM_LIBRARIES}
        SQLite::SQLite3
        Boost::program_options
    )
    install(TARGETS goodnet_store RUNTIME DESTINATION bin)
endif()

# ─── 5. Tests ────────────────────────────────────────────────────────────────

if(BUILD_TESTING AND GTest_FOUND)
    enable_testing()

    add_executable(unit_tests
        tests/core.cpp
        tests/conf.cpp
        tests/connection_manager.cpp
        tests/queue_stress.cpp
        tests/signals.cpp
        tests/heartbeat.cpp
        tests/logger.cpp
    )

    set_target_properties(unit_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Mock plugins require dynamic loading (dlopen/LoadLibrary).
    # With GOODNET_STATIC_PLUGINS the plugin tests are skipped.
    if(NOT GOODNET_STATIC_PLUGINS)
        add_library(mock_handler SHARED tests/mock_handler.cpp)
        target_link_libraries(mock_handler PRIVATE goodnet_core)

        add_library(mock_connector SHARED tests/mock_connector.cpp)
        target_link_libraries(mock_connector PRIVATE goodnet_core)

        target_sources(unit_tests PRIVATE tests/plugins.cpp)
        target_compile_definitions(unit_tests PRIVATE
            MOCK_HANDLER_PATH="$<TARGET_FILE:mock_handler>"
            MOCK_CONNECTOR_PATH="$<TARGET_FILE:mock_connector>"
        )
    endif()

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