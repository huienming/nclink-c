# NC-Link adapter layer: the host program that loads adapters, on top of the
# core library and the protocol clients.
#
# Two layers, on purpose:
#
#   nclink_drivers   the static library: driver interface, configuration,
#                    model generation, MQTT session, audit, adapters.
#   ncl_adapter      the device program: one ncl_server plus whatever the
#                    configuration loads.
#
# The protocol implementations themselves live in ../clients (Modbus, MC,
# FINS, S7, FOCAS ...): they are the reusable half a site-specific adapter
# talks through, so an adapter never carries protocol bytes.
#
# Plugins/ below is where the *adapters* live: one .c file each, and every file
# is built as plugins/ncl_driver_<name>.dll|.so that the host loads at start-up
# (see include/nclink/ncl_tool.h). Adding a brand is dropping a file in there -
# no CMake edit, which is the whole point of the one-file adapter.

file(GLOB_RECURSE NCLINK_ADAPTER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.c")

add_library(nclink_drivers STATIC ${NCLINK_ADAPTER_SOURCES})
add_library(nclink::drivers ALIAS nclink_drivers)
set_target_properties(nclink_drivers PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(nclink_drivers
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(nclink_drivers PUBLIC nclink::core nclink::clients)

# One file, one adapter: every adapters/plugins/<name>.c becomes the module
# plugins/ncl_driver_<name>.dll (libncl_driver_<name>.so), which the host finds
# by the name the configuration uses.
if(NCLINK_BUILD_PLUGINS)
    file(GLOB NCLINK_PLUGIN_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/plugins/*.c")
    foreach(source IN LISTS NCLINK_PLUGIN_SOURCES)
        get_filename_component(name "${source}" NAME_WE)
        set(target "ncl_driver_${name}")
        add_library(${target} MODULE "${source}")
        target_link_libraries(${target} PRIVATE nclink_drivers)
        target_include_directories(${target} PRIVATE
                "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/clients")
        set_target_properties(${target} PROPERTIES
                OUTPUT_NAME "ncl_driver_${name}"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /wd4996 /utf-8)
            target_compile_definitions(${target} PRIVATE
                    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow
                    -Wstrict-prototypes -Wmissing-prototypes)
        endif()
    endforeach()
    message(STATUS "nclink: adapters are built as modules -> "
            "${CMAKE_BINARY_DIR}/plugins")
endif()

if(MSVC)
    target_compile_options(nclink_drivers PRIVATE /W4 /wd4996 /utf-8)
    target_compile_definitions(nclink_drivers PRIVATE
            _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_compile_options(nclink_drivers PRIVATE -Wall -Wextra -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes)
endif()

if(NCLINK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# The daemon itself: src/main.c is not part of the library.
add_executable(ncl_adapter src/main.c)
target_link_libraries(ncl_adapter PRIVATE nclink_drivers)
# src/ is where the internal headers (core/adapter_text.h) live.
target_include_directories(ncl_adapter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(MSVC)
    target_compile_options(ncl_adapter PRIVATE /W4 /wd4996 /utf-8)
    target_compile_definitions(ncl_adapter PRIVATE
            _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_compile_options(ncl_adapter PRIVATE -Wall -Wextra -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes)
endif()
