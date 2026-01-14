cmake_minimum_required(VERSION 3.22.1)
project(GoodNetHandlers)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/libs")

# Добавляем логгер из основного проекта
add_library(logger_plugin SHARED
    logger.cpp 
    ../../src/logger.cpp
)

set_target_properties(logger_plugin PROPERTIES OUTPUT_NAME "logger" PREFIX "lib")

target_include_directories(logger_plugin PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
)

target_link_libraries(logger_plugin PRIVATE fmt::fmt spdlog::spdlog)
