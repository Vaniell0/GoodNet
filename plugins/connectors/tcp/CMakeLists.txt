cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 23)
project(goodnet-tcp)

find_package(GoodNet REQUIRED)
find_package(Boost REQUIRED COMPONENTS)

add_plugin(tcp tcp.cpp)

target_link_libraries(tcp
  PRIVATE
    GoodNet::core
)
