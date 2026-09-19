# NC-Link adapter: the vendor protocol drivers, on top of the core library.
#
# A static library plus, later, the "ncl_adapter" daemon that turns a driver
# config into a live NC-Link device model. Kept apart from nclink_core so a
# device build can leave the drivers out entirely.

file(GLOB_RECURSE NCLINK_ADAPTER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/registry/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/drivers/*/*.c")

add_library(nclink_drivers STATIC ${NCLINK_ADAPTER_SOURCES})
add_library(nclink::drivers ALIAS nclink_drivers)
set_target_properties(nclink_drivers PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(nclink_drivers
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
               "${CMAKE_CURRENT_SOURCE_DIR}/drivers"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(nclink_drivers PUBLIC nclink::core)

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

if(MSVC)
    target_compile_options(ncl_adapter PRIVATE /W4 /wd4996 /utf-8)
    target_compile_definitions(ncl_adapter PRIVATE
            _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_compile_options(ncl_adapter PRIVATE -Wall -Wextra -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes)
endif()
