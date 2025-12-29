cmake_minimum_required(VERSION 3.22.1)

if (NOT PLUGIN_BUILD)
    return()
endif()

project(LoggerHandler)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(logger_handler SHARED logger.cpp)

set_target_properties(logger_handler PROPERTIES
    OUTPUT_NAME "logger"
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../libs"
)

target_include_directories(logger_handler PRIVATE
    ${CMAKE_SOURCE_DIR}/../../sdk
)
