cmake_minimum_required(VERSION 3.22)
set(CMAKE_CXX_STANDARD 23)
project(goodnet-logger)

find_package(GoodNet REQUIRED)
include(${GOODNET_SDK_HELPER})

find_package(spdlog REQUIRED)

add_plugin(logger logger.cpp)

# Линкуем зависимости
target_link_libraries(logger
  PRIVATE
    spdlog::spdlog
)
