cmake_minimum_required(VERSION 3.22.1)
project(GoodNet)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_AS_LIBRARY "Build core as library for export" OFF)
option(INSTALL_DEVELOPMENT "Install SDK files" OFF)

# Нужные зависимости
cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)

# --- 1. SDK (Интерфейсы для плагинов) ---
add_library(goodnet_sdk INTERFACE)
add_library(GoodNet::sdk ALIAS goodnet_sdk)

target_include_directories(goodnet_sdk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk/cpp>
    # В установленном виде ищем здесь:
    $<INSTALL_INTERFACE:include/goodnet/sdk>
    $<INSTALL_INTERFACE:include/goodnet/sdk/cpp>
)

# --- 2. CORE (Реализация) ---
add_library(goodnet_core STATIC
    src/core.cpp

    src/config.cpp
    src/logger.cpp
    src/signals.cpp
    
    core/homeServices.cpp
    core/pluginManager.cpp
    core/connectManager.cpp
)

target_include_directories(goodnet_core PRIVATE 
    ${CMAKE_SOURCE_DIR}/core
)

add_library(GoodNet::core ALIAS goodnet_core)
set_target_properties(goodnet_core PROPERTIES EXPORT_NAME core)

target_link_libraries(goodnet_core PUBLIC goodnet_sdk)

target_include_directories(goodnet_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/goodnet/include>
)

# Внешние зависимости
target_link_libraries(goodnet_core PUBLIC 
    nlohmann_json::nlohmann_json spdlog::spdlog fmt::fmt Boost::system
)
target_link_libraries(goodnet_core PRIVATE Boost::system sodium dl)

# --- 3. APP ---
add_executable(goodnet src/main.cpp)
target_link_libraries(goodnet PRIVATE goodnet_core)

# --- 4. УСТАНОВКА (Фикс путей) ---
if(INSTALL_DEVELOPMENT)
    # 1. Устанавливаем SDK в include/goodnet/sdk
    install(DIRECTORY sdk/ DESTINATION include/goodnet/sdk)
    
    # 2. Устанавливаем заголовки Core в include/goodnet
    # (Содержимое папки include/ копируется ВНУТРЬ include/goodnet)
    install(DIRECTORY include/ DESTINATION include/goodnet/include)

    # 3. Хелпер
    install(FILES plugins/helper.cmake DESTINATION lib/cmake/GoodNet)

    # 4. Экспорт целей
    # INCLUDES DESTINATION автоматически добавляет путь к интерфейсу
    install(TARGETS goodnet_core goodnet_sdk 
            EXPORT GoodNetTargets 
            ARCHIVE DESTINATION lib 
            LIBRARY DESTINATION lib
            INCLUDES DESTINATION include/goodnet) 

    install(EXPORT GoodNetTargets
            FILE GoodNetTargets.cmake
            NAMESPACE GoodNet::
            DESTINATION lib/cmake/GoodNet)

    # 5. Конфиг
    file(WRITE "${CMAKE_BINARY_DIR}/GoodNetConfig.cmake" "
include(CMakeFindDependencyMacro)
cmake_policy(SET CMP0167 NEW)
find_dependency(Boost REQUIRED COMPONENTS system filesystem)
find_dependency(nlohmann_json REQUIRED)
find_dependency(spdlog REQUIRED)
find_dependency(fmt REQUIRED)

include(\${CMAKE_CURRENT_LIST_DIR}/GoodNetTargets.cmake)
set(GOODNET_SDK_HELPER \"\${CMAKE_CURRENT_LIST_DIR}/helper.cmake\")
")
    install(FILES "${CMAKE_BINARY_DIR}/GoodNetConfig.cmake" DESTINATION lib/cmake/GoodNet)
endif()

install(TARGETS goodnet DESTINATION bin)
