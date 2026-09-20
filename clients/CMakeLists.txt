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


if(NCLINK_BUILD_TESTS)
    add_subdirectory(tests)
endif()