cmake_minimum_required(VERSION 3.22.1)
project(GoodNetConnectors)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

cmake_policy(SET CMP0167 NEW)
if(NOT TARGET Boost::system)
    find_package(Boost REQUIRED COMPONENTS system)
endif()
if(NOT TARGET fmt::fmt)
    find_package(fmt REQUIRED)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/libs")
file(MAKE_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")

add_library(tcp_connector SHARED tcp.cpp)
set_target_properties(tcp_connector PROPERTIES OUTPUT_NAME "tcp" PREFIX "lib")

# Правильные пути к SDK относительно папки plugins/connectors
target_include_directories(tcp_connector PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
)

target_link_libraries(tcp_connector PRIVATE Boost::system fmt::fmt)