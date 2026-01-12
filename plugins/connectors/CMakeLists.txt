cmake_minimum_required(VERSION 3.22.1)

if (NOT PLUGIN_BUILD)
    return()
endif()

project(TcpConnector)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ищем Boost
cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED COMPONENTS system)

# Создаем shared library для плагина
add_library(tcp_connector SHARED tcp.cpp)

# Настройки для плагина
set_target_properties(tcp_connector PROPERTIES
    OUTPUT_NAME "tcp"
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../libs"
)

# Включаем директории SDK
target_include_directories(tcp_connector PRIVATE
    ${CMAKE_SOURCE_DIR}/../../sdk
)

# Ссылки
target_link_libraries(tcp_connector PRIVATE 
    Boost::system
)
