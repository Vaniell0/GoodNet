cmake_minimum_required(VERSION 3.22.1)
project(GoodNet)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Опция для включения сборки плагинов
option(BUILD_PLUGINS "Build plugins" OFF)

# Поиск необходимых библиотек
cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED COMPONENTS system filesystem)
find_package(fmt REQUIRED)
find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(sodium_PKG QUIET libsodium)
endif()

# Исполняемый файл
add_executable(goodnet
    src/main.cpp

    src/logger.cpp
)

# Корневой каталог для включений
target_include_directories(goodnet PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sdk
)

set(EXECUTABLE_OUTPUT_PATH "${CMAKE_BINARY_DIR}/bin")

# Ссылки на библиотеки
target_link_libraries(goodnet PRIVATE
    Boost::system
    Boost::filesystem
    fmt::fmt
    sodium dl
)

# Создаем директории для плагинов
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/handlers)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/connectors)

# Определяем путь к плагинам для тестирования
target_compile_definitions(goodnet PRIVATE
    PLUGINS_DIR="${CMAKE_BINARY_DIR}/plugins"
)

# Опциональная сборка плагинов
if (BUILD_PLUGINS)
    message(STATUS "Building plugins...")
    
    # Плагин-обработчик логов
    add_subdirectory(plugins/handlers)
    
    # Плагин-коннектор (tcp)
    add_subdirectory(plugins/connectors)
endif()

# Установка
install(TARGETS goodnet DESTINATION bin)
install(DIRECTORY include/sdk DESTINATION include/goodnet)
