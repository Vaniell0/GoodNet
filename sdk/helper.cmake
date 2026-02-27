# Если пути не переданы извне (Nix), пытаемся найти их относительно
if(NOT DEFINED GOODNET_SDK_PATH)
    get_filename_component(_POSSIBLE_SDK "${CMAKE_CURRENT_LIST_DIR}/../sdk" ABSOLUTE)
    if(EXISTS "${_POSSIBLE_SDK}")
        set(GOODNET_SDK_PATH "${_POSSIBLE_SDK}")
        set(GOODNET_INC_PATH "${CMAKE_CURRENT_LIST_DIR}/../include")
    else()
        # Вариант, когда хелпер лежит внутри установленного SDK
        set(GOODNET_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/sdk")
        set(GOODNET_INC_PATH "${CMAKE_CURRENT_LIST_DIR}/include")
    endif()
endif()

message(STATUS "🔧 Plugin Builder | SDK: ${GOODNET_SDK_PATH}")
message(STATUS "🔧 Plugin Builder | Inc: ${GOODNET_INC_PATH}")

macro(add_plugin NAME)
    add_library(${NAME} SHARED ${ARGN})

    set_target_properties(${NAME} PROPERTIES 
        PREFIX "lib"
        CXX_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    # Подключаем заголовки из установленного goodnet-core
    target_include_directories(${NAME} PRIVATE 
        ${GOODNET_INC_PATH}
        ${GOODNET_SDK_PATH}
        ${GOODNET_SDK_PATH}/cpp
    )

    # Если есть дополнительные заголовки в core
    if(EXISTS "${GOODNET_INC_PATH}/goodnet/core")
        target_include_directories(${NAME} PRIVATE 
            ${GOODNET_INC_PATH}/goodnet/core
        )
    endif()

    # Оптимизации
    target_compile_options(${NAME} PRIVATE -Os -ffunction-sections -fdata-sections)
    if(NOT APPLE)
        target_link_options(${NAME} PRIVATE -Wl,--gc-sections)
    endif()
endmacro()