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

# ─── PCH (только в Debug) ─────────────────────────────────────────────────────
# Ускоряет инкрементальные пересборки в nix develop + cmake --build build/debug
# В Release не нужен: Nix кеширует деривацию целиком, перкомпиляция единичных
# файлов не происходит.
include(cmake/pch.cmake)

# ─── 1. SDK ───────────────────────────────────────────────────────────────────

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
# SHARED обязателен — одна копия всех static переменных (Logger, once_flag).
# Плагин через dlopen(RTLD_GLOBAL) видит тот же экземпляр Logger что и ядро.
# STATIC → дублирование static в executable + в плагинах → double init → SIGSEGV.

add_library(goodnet_core SHARED
    src/config.cpp
    src/logger.cpp
    core/pluginManager_core.cpp
    core/pluginManager_state.cpp
    core/pluginManager_lookup.cpp
)

add_library(GoodNet::core ALIAS goodnet_core)
set_target_properties(goodnet_core PROPERTIES
    EXPORT_NAME core
    VERSION     0.1.0
    SOVERSION   0
)

target_link_libraries(goodnet_core PUBLIC goodnet_sdk)

target_include_directories(goodnet_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core>
        $<INSTALL_INTERFACE:include/goodnet>
)

target_link_libraries(goodnet_core
    # PUBLIC: spdlog и fmt используются в logger.hpp (публичный заголовок).
    # Потребители (плагины) не линкуют их напрямую, но cmake экспортирует
    # их как INTERFACE_LINK_LIBRARIES → find_dependency в GoodNetConfig.cmake.
    PUBLIC  spdlog::spdlog fmt::fmt

    # PRIVATE: используются только в .cpp файлах ядра, не в публичных заголовках.
    # НЕ попадают в GoodNetTargets.cmake INTERFACE_LINK_LIBRARIES.
    # Плагинам не нужно их находить при find_package(GoodNet).
    PRIVATE nlohmann_json::nlohmann_json Boost::system sodium
)
set_target_properties(goodnet_core PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(goodnet_core PUBLIC SPDLOG_COMPILED_LIB)

# GOODNET_PROJECT_ROOT: CMake SOURCE_DIR → относительные пути в логах.
# [src/config.cpp:42] вместо [/nix/store/...-source/src/config.cpp:42]
# PRIVATE: compile-time константа, не нужна зависимым целям.
target_compile_definitions(goodnet_core PRIVATE
    GOODNET_PROJECT_ROOT="${CMAKE_SOURCE_DIR}/"
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

if(BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED)

    add_library(mock_handler SHARED tests/mock_handler.cpp)
    target_link_libraries(mock_handler PRIVATE goodnet_core)
    
    add_library(mock_connector SHARED tests/mock_connector.cpp)
    target_link_libraries(mock_connector PRIVATE goodnet_core)

    add_executable(unit_tests
        tests/conf.cpp
        tests/plugins.cpp
    )
    
    set_target_properties(unit_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    
    target_compile_definitions(unit_tests PRIVATE 
        MOCK_HANDLER_PATH="$<TARGET_FILE:mock_handler>"
        MOCK_CONNECTOR_PATH="$<TARGET_FILE:mock_connector>"
    )

    target_link_libraries(unit_tests PRIVATE
        goodnet_core
        GTest::gmock_main
        sodium
    )

    target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME AllTests COMMAND unit_tests)
endif()
