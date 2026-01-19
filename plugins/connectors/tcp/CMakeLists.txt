cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 23)
project(goodnet-tcp)

find_package(GoodNet REQUIRED)
include(${GOODNET_SDK_HELPER})

cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED COMPONENTS system)

add_plugin(tcp tcp.cpp)

target_link_libraries(tcp
  PRIVATE
    Boost::system
)
