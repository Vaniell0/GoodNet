message(STATUS "🔧 Plugin Builder | SDK Headers: ${GoodNet_INCLUDE_DIRS}")

macro(add_plugin NAME)
    add_library(${NAME} SHARED ${ARGN})
    
    set_target_properties(${NAME} PROPERTIES 
        PREFIX "lib"
        CXX_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
    
    if(TARGET GoodNet::sdk)
        target_link_libraries(${NAME} PRIVATE GoodNet::sdk)
    endif()
    
    if(TARGET GoodNet::core)
        target_link_libraries(${NAME} PRIVATE GoodNet::core)
    endif()

    # Оптимизации
    target_compile_options(${NAME} PRIVATE -Os -ffunction-sections -fdata-sections)
    if(NOT APPLE)
        target_link_options(${NAME} PRIVATE -Wl,--gc-sections)
    endif()

    install(TARGETS ${NAME} 
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
            RUNTIME DESTINATION bin)
    # --------------------
endmacro()
