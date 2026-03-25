add_library(template_handler SHARED template_handler.cpp)
target_link_libraries(template_handler PRIVATE goodnet_sdk)
target_include_directories(template_handler PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk/cpp
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/include
)
set_target_properties(template_handler PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
