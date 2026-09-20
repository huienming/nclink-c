# NC-Link adapter layer: the host program that loads adapters, on top of the
# core library and the protocol clients.
#
# Two layers, on purpose:
#
#   nclink_drivers   the static library: driver interface, configuration,
#                    point map, model generation, MQTT session, audit.
#   ncl_adapter      the device program: one ncl_server plus whatever the
#                    configuration loads.
#
# The protocol implementations themselves live in ../clients (Modbus, MC,
# FINS, S7, FOCAS ...): they are the reusable half a site-specific adapter
# talks through, so an adapter never carries protocol bytes.
#
# A client can also arrive as a *module* (ncl_driver_<protocol>.dll / .so) that
# the program loads at start-up; see ../clients/CMakeLists.txt and
# include/nclink_adapter/ncl_module.h. A client built as a module is left out
# of the built-in registry, so the module is what registers it.

file(GLOB_RECURSE NCLINK_ADAPTER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/registry/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.c")

add_library(nclink_drivers STATIC ${NCLINK_ADAPTER_SOURCES})
add_library(nclink::drivers ALIAS nclink_drivers)
set_target_properties(nclink_drivers PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(nclink_drivers
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(nclink_drivers PUBLIC nclink::core nclink::clients)

if(MSVC)
    target_compile_options(nclink_drivers PRIVATE /W4 /wd4996 /utf-8)
    target_compile_definitions(nclink_drivers PRIVATE
            _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_compile_options(nclink_drivers PRIVATE -Wall -Wextra -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes)
endif()

# Protocol clients as loadable modules: the device program picks them up from
# its plugin directory, so "which brands this box speaks" is a deployment
# decision. Turning this off puts every client back into the built-in registry
# (one self contained binary, no plugin directory). The option itself is
# declared in the root CMakeLists.txt (the clients subdirectory is configured
# first and builds the module).
if(NCLINK_BUILD_PLUGINS)
    target_compile_definitions(nclink_drivers PRIVATE
            NCL_DRIVER_FOCAS_IS_PLUGIN=1)
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
