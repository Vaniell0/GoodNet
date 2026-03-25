cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 23)
project(goodnet-ice)

find_package(GoodNet REQUIRED)
find_package(Boost REQUIRED COMPONENTS)
find_package(OpenSSL REQUIRED)

# ── Plugin target ─────────────────────────────────────────────────────────────
add_plugin(ice
    ice.cpp
    stun.cpp
    session.cpp
)

target_include_directories(ice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ice
  PRIVATE
    GoodNet::core
    OpenSSL::Crypto
)
