cmake_minimum_required(VERSION 3.22.1)
project(GoodNet)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Опции сборки
option(BUILD_AS_LIBRARY "Build as library for development" OFF)
option(INSTALL_DEVELOPMENT "Install development files" OFF)

# --- Зависимости Core ---
cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

# Настройка путей вывода
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# --- 1. Собираем ядро (всегда) ---
add_executable(goodnet
    src/main.cpp
    # src/core.cpp
    # src/config.cpp
    # src/logger.cpp
    # src/signals.cpp

    # core/homeServices.cpp
    # core/pluginManager.cpp
    # core/connectManager.cpp
)

target_include_directories(goodnet PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/sdk
)

target_link_libraries(goodnet PRIVATE
    nlohmann_json::nlohmann_json
    Boost::filesystem
    spdlog::spdlog
    Boost::system
    sodium dl
    fmt::fmt
)

# --- 2. Если собираем как библиотеку ---
if(BUILD_AS_LIBRARY)
    add_library(goodnet_core STATIC
        src/config.cpp
        src/logger.cpp
        # src/signals.cpp

        # core/homeServices.cpp
        # core/pluginManager.cpp
        # core/connectManager.cpp
    )

    target_include_directories(goodnet_core PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/core
        ${CMAKE_SOURCE_DIR}/sdk
    )

    target_link_libraries(goodnet_core PUBLIC
        nlohmann_json::nlohmann_json
        Boost::filesystem
        spdlog::spdlog
        Boost::system
        sodium dl
        fmt::fmt
    )
endif()

# --- Установка (в зависимости от режима) ---
if(INSTALL_DEVELOPMENT)
    # Режим библиотеки: устанавливаем ВСЁ для разработки плагинов

    # 1. Устанавливаем заголовки ядра
    install(DIRECTORY include/ DESTINATION include)

    # 2. Устанавливаем SDK для плагинов
    install(DIRECTORY sdk/ DESTINATION sdk)

    # 3. Устанавливаем заголовки core
    install(DIRECTORY core/ DESTINATION core FILES_MATCHING PATTERN "*.hpp")

    # 4. Устанавливаем заголовки из core как часть SDK (для совместимости)
    install(DIRECTORY core/ DESTINATION include/goodnet/core FILES_MATCHING PATTERN "*.hpp")

    if(BUILD_AS_LIBRARY)
        # 5. Устанавливаем статическую библиотеку
        install(TARGETS goodnet_core
            ARCHIVE DESTINATION lib
            INCLUDES DESTINATION include
        )
    endif()

    message(STATUS "📦 Building GoodNet as LIBRARY for development")
else()
    # Режим приложения: только приложение
    install(TARGETS goodnet DESTINATION bin)
    message(STATUS "Building GoodNet as APPLICATION")
endif()
