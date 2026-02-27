cmake_minimum_required(VERSION 3.22.1)
project(logger LANGUAGES CXX)

# Подключаем helper.cmake из SDK
include(${GOODNET_SDK_PATH}/helper.cmake)

add_plugin(logger
    logger.cpp
)

find_package(spdlog REQUIRED)
target_link_libraries(logger PRIVATE
    spdlog::spdlog
)