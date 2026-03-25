add_library(template_connector SHARED template_connector.cpp)
target_link_libraries(template_connector PRIVATE goodnet_sdk)
target_include_directories(template_connector PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk/cpp
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/include
)
set_target_properties(template_connector PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
