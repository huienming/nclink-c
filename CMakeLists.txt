cmake_minimum_required(VERSION 3.16)

project(nclink-core-c
        VERSION 3.0.0
        DESCRIPTION "NC-Link core middleware - C implementation (GB/T 41970-2022)"
        LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Sources and string literals are UTF-8 (log messages and device descriptions
# contain Chinese text), so the execution charset must be UTF-8 regardless of
# the machine's code page. Applies to the library and to the tests, which
# assert on those strings.
if(MSVC)
    add_compile_options(/utf-8)
else()
    # -std=c11 hides the POSIX interfaces the library needs (strdup,
    # getaddrinfo, localtime_r, pthread_*), so ask for them explicitly.
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
    # Fixed size path buffers (NCL_PATH_MAX_BUF) make GCC's format-truncation
    # analysis very noisy; the truncation there is deliberate and bounded.
    add_compile_options(-Wno-format-truncation)
endif()

option(NCLINK_BUILD_TESTS "Build nclink unit tests" ON)
option(NCLINK_BUILD_EXAMPLES "Build the example programs" ON)
option(NCLINK_WITH_ZLIB "Enable zlib based compress codec" OFF)
option(NCLINK_WITH_MQTT "Build the MQTT 5.0 transport layer" ON)
option(NCLINK_WITH_TLS "Enable TLS (OpenSSL) for MQTT over ssl://" OFF)
option(NCLINK_BUILD_CPP "Build the C++ wrapper sample/test (C++17)" ON)

# ---------------------------------------------------------------- library ----
file(GLOB_RECURSE NCLINK_CORE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/general/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/message/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/codec/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/http/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/rest/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/config/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ftp/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/schema/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/model/*.c")

add_library(nclink_core STATIC ${NCLINK_CORE_SOURCES})
add_library(nclink::core ALIAS nclink_core)

target_include_directories(nclink_core
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

find_package(Threads REQUIRED)
target_link_libraries(nclink_core PUBLIC Threads::Threads)

if(NCLINK_WITH_ZLIB)
    find_package(ZLIB REQUIRED)
    target_compile_definitions(nclink_core PUBLIC NCL_HAVE_ZLIB=1)
    target_link_libraries(nclink_core PRIVATE ZLIB::ZLIB)
endif()

if(NCLINK_WITH_TLS)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(nclink_core PUBLIC NCL_WITH_TLS=1)
    target_link_libraries(nclink_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(MSVC)
    # /utf-8: sources and string literals are UTF-8. The library emits UTF-8
    # JSON (log messages and device descriptions contain Chinese text), so the
    # execution charset must be UTF-8 regardless of the machine's code page.
    target_compile_options(nclink_core PRIVATE /W4 /wd4996 /utf-8)
    target_compile_definitions(nclink_core PRIVATE _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_compile_options(nclink_core PRIVATE -Wall -Wextra -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes)
endif()

# ------------------------------------------------------------------ tests ----
if(NCLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ------------------------------------------------------------------- apps ----
if(NCLINK_WITH_MQTT)
    file(GLOB_RECURSE NCLINK_TRANSPORT_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/mqtt/*.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/client/*.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/server/*.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/file/*.c")
    if(NCLINK_TRANSPORT_SOURCES)
        target_sources(nclink_core PRIVATE ${NCLINK_TRANSPORT_SOURCES})
    endif()
endif()

# --------------------------------------------------------------- examples ----
if(NCLINK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
