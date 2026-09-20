# NC-Link protocol clients: the vendor protocol implementations, on top of the
# core library and independent of the adapter layer.
#
#   nclink_clients   the static library: one directory per protocol (modbus,
#                    mc, fins, s7, mtconnect, meldas, lsv2, syntec, knd, mock,
#                    focas) plus the small HTTP client the URL based ones
#                    share.
#
# A client is the reusable half: it speaks bytes and sessions (Modbus frames,
# FOCAS blocks, ...). An *adapter* is the device-specific program in
# ../adapters: it decides which points exist and what they mean, and is what
# the host (ncl_adapter) loads. Keeping the two apart is what lets the same
# FOCAS/Modbus/... code serve every site with a different point map.
#
# Headers are public under include/nclink/clients/<protocol>.h; the internal
# ones stay next to their sources and are reached with -Iclients.
#
# The FOCAS client is additionally built as a loadable module
# (plugins/ncl_driver_focas.dll|.so) while the adapter-as-module story is still
# being built out; see NCLINK_BUILD_PLUGINS.

file(GLOB_RECURSE NCLINK_CLIENT_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*/*.c")
# Tests are their own targets (tests/CMakeLists.txt); a module entry point is
# compiled into the module, not into the library.
list(FILTER NCLINK_CLIENT_SOURCES EXCLUDE REGEX "/tests/")
list(FILTER NCLINK_CLIENT_SOURCES EXCLUDE REGEX "_plugin\\.c$")

add_library(nclink_clients STATIC ${NCLINK_CLIENT_SOURCES})
add_library(nclink::clients ALIAS nclink_clients)
set_target_properties(nclink_clients PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(nclink_clients
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
               "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(nclink_clients PUBLIC nclink::core)

if(MSVC)
    target_compile_options(nclink_clients PRIVATE /W4 /wd4996 /utf-8)
    target_compile_definitions(nclink_clients PRIVATE
            _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_compile_options(nclink_clients PRIVATE -Wall -Wextra -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes)
endif()

# The FOCAS client as a loadable module, until adapters take that role.
# It links nclink_drivers rather than nclink_clients because a client is still
# an ncl_driver_ops implementation: the ops runtime (ncl_driver_new,
# ncl_driver_result_*, the error tiers) lives in the adapter layer until the
# clients get their own idiomatic API.
if(NCLINK_BUILD_PLUGINS)
    add_library(ncl_driver_focas MODULE
            focas/focas_driver.c
            focas/focas_codec.c
            focas/focas_plugin.c)
    set_target_properties(ncl_driver_focas PROPERTIES
            OUTPUT_NAME "ncl_driver_focas"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
    target_link_libraries(ncl_driver_focas PRIVATE nclink_drivers)
    if(MSVC)
        target_compile_options(ncl_driver_focas PRIVATE /W4 /wd4996 /utf-8)
        target_compile_definitions(ncl_driver_focas PRIVATE
                _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    else()
        target_compile_options(ncl_driver_focas PRIVATE -Wall -Wextra -Wshadow
                -Wstrict-prototypes -Wmissing-prototypes)
    endif()
    message(STATUS "nclink: protocol clients are built as plugins -> "
            "${CMAKE_BINARY_DIR}/plugins")
endif()

if(NCLINK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
