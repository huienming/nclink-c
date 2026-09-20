add_executable(ncl_test_json test_json.c)
target_link_libraries(ncl_test_json PRIVATE nclink_core)
add_test(NAME json COMMAND ncl_test_json)

add_executable(ncl_test_common test_common.c)
target_link_libraries(ncl_test_common PRIVATE nclink_core)
add_test(NAME common COMMAND ncl_test_common)

# Allocation seam: the same binary passes with the runtime allocator and with
# the NCL_STATIC_MEM pool (the pooling specific checks need NCL_STATIC_MEM).
add_executable(ncl_test_mem test_mem.c)
target_link_libraries(ncl_test_mem PRIVATE nclink_core)
if(NCLINK_STATIC_MEM)
    target_compile_definitions(ncl_test_mem PRIVATE
            NCL_STATIC_MEM=1
            NCL_MEM_POOL_BYTES=${NCLINK_MEM_POOL_BYTES})
endif()
add_test(NAME mem COMMAND ncl_test_mem)

# Monte Carlo pressure test: random sizes and random operation sequences, with
# a replay of every seed to catch drift. Runs against the pool in
# NCLINK_STATIC_MEM builds and against the runtime allocator otherwise.
add_executable(ncl_test_mem_mc test_mem_mc.c)
target_link_libraries(ncl_test_mem_mc PRIVATE nclink_core)
if(NCLINK_STATIC_MEM)
    target_compile_definitions(ncl_test_mem_mc PRIVATE
            NCL_STATIC_MEM=1
            NCL_MEM_POOL_BYTES=${NCLINK_MEM_POOL_BYTES})
endif()
add_test(NAME mem_mc COMMAND ncl_test_mem_mc)
set_tests_properties(mem_mc PROPERTIES TIMEOUT 300)

# Multithreaded pressure test: several threads hammering one pool, handing
# ownership of blocks across threads, with the main thread checking the pool
# invariants while they run. NCL_MEM_MT_SECONDS turns it into a soak.
add_executable(ncl_test_mem_mt test_mem_mt.c)
target_link_libraries(ncl_test_mem_mt PRIVATE nclink_core)
if(NCLINK_STATIC_MEM)
    target_compile_definitions(ncl_test_mem_mt PRIVATE
            NCL_STATIC_MEM=1
            NCL_MEM_POOL_BYTES=${NCLINK_MEM_POOL_BYTES})
    if(NCLINK_MEM_SINGLE_THREAD)
        target_compile_definitions(ncl_test_mem_mt PRIVATE NCL_MEM_SINGLE_THREAD=1)
    endif()
endif()
add_test(NAME mem_mt COMMAND ncl_test_mem_mt)
set_tests_properties(mem_mt PROPERTIES TIMEOUT 600)

add_executable(ncl_test_topic test_topic.c)
target_link_libraries(ncl_test_topic PRIVATE nclink_core)
add_test(NAME topic COMMAND ncl_test_topic)

# Dynamic library loading: the fixture module next to the test binary stands in
# for a vendor adapter, so the loader is exercised against a real .dll/.so.
add_library(ncl_test_module MODULE test_module.c)
set_target_properties(ncl_test_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "ncl_test_module")
add_executable(ncl_test_library test_library.c)
target_link_libraries(ncl_test_library PRIVATE nclink_core)
target_compile_definitions(ncl_test_library PRIVATE
        NCL_TEST_MODULE_DIR="$<TARGET_FILE_DIR:ncl_test_module>")
add_dependencies(ncl_test_library ncl_test_module)
add_test(NAME library COMMAND ncl_test_library)

add_executable(ncl_test_model test_model.c)
target_link_libraries(ncl_test_model PRIVATE nclink_core)
target_compile_definitions(ncl_test_model PRIVATE
        NCL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME model COMMAND ncl_test_model)

add_executable(ncl_test_message test_message.c)
target_link_libraries(ncl_test_message PRIVATE nclink_core)
target_compile_definitions(ncl_test_message PRIVATE
        NCL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME message COMMAND ncl_test_message)

add_executable(ncl_test_codec test_codec.c)
target_link_libraries(ncl_test_codec PRIVATE nclink_core)
add_test(NAME codec COMMAND ncl_test_codec)

add_executable(ncl_test_thread test_thread.c)
target_link_libraries(ncl_test_thread PRIVATE nclink_core)
add_test(NAME thread COMMAND ncl_test_thread)

add_executable(ncl_test_mqtt test_mqtt.c)
target_link_libraries(ncl_test_mqtt PRIVATE nclink_core)
add_test(NAME mqtt COMMAND ncl_test_mqtt)

add_executable(ncl_test_mqtt_client test_mqtt_client.c)
target_link_libraries(ncl_test_mqtt_client PRIVATE nclink_core)
add_test(NAME mqtt_client COMMAND ncl_test_mqtt_client)
set_tests_properties(mqtt_client PROPERTIES TIMEOUT 60)

add_executable(ncl_test_client test_client.c fake_nclink_server.c)
target_link_libraries(ncl_test_client PRIVATE nclink_core)
target_include_directories(ncl_test_client PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ncl_test_client PRIVATE
        NCL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME client COMMAND ncl_test_client)
set_tests_properties(client PROPERTIES TIMEOUT 90)

add_executable(ncl_test_server test_server.c fake_nclink_server.c)
target_link_libraries(ncl_test_server PRIVATE nclink_core)
target_include_directories(ncl_test_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME server COMMAND ncl_test_server)
set_tests_properties(server PROPERTIES TIMEOUT 90)

add_executable(ncl_test_http test_http.c)
target_link_libraries(ncl_test_http PRIVATE nclink_core)
add_test(NAME http COMMAND ncl_test_http)
set_tests_properties(http PROPERTIES TIMEOUT 60)

add_executable(ncl_test_rest test_rest.c)
target_link_libraries(ncl_test_rest PRIVATE nclink_core)
add_test(NAME rest COMMAND ncl_test_rest)
set_tests_properties(rest PROPERTIES TIMEOUT 60)

add_executable(ncl_test_config test_config.c)
target_link_libraries(ncl_test_config PRIVATE nclink_core)
add_test(NAME config COMMAND ncl_test_config)
set_tests_properties(config PROPERTIES TIMEOUT 60)

add_executable(ncl_test_ftp test_ftp.c)
target_link_libraries(ncl_test_ftp PRIVATE nclink_core)
add_test(NAME ftp COMMAND ncl_test_ftp)
set_tests_properties(ftp PROPERTIES TIMEOUT 120)

add_executable(ncl_test_file test_file.c)
target_link_libraries(ncl_test_file PRIVATE nclink_core)
add_test(NAME file COMMAND ncl_test_file)
set_tests_properties(file PROPERTIES TIMEOUT 180)

add_executable(ncl_test_schema test_schema.c)
target_link_libraries(ncl_test_schema PRIVATE nclink_core)
add_test(NAME schema COMMAND ncl_test_schema)
set_tests_properties(schema PROPERTIES TIMEOUT 60)

add_executable(ncl_test_event test_event.c)
target_link_libraries(ncl_test_event PRIVATE nclink_core)
add_test(NAME event COMMAND ncl_test_event)
set_tests_properties(event PROPERTIES TIMEOUT 60)

add_executable(ncl_test_license test_license.c)
target_link_libraries(ncl_test_license PRIVATE nclink_core)
target_compile_definitions(ncl_test_license PRIVATE
        NCL_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME license COMMAND ncl_test_license)

# Interoperability against a real broker: skipped unless NCL_TEST_MQTT_BROKER
# is set (tools/interop.sh starts one in Docker and runs this suite).
add_executable(ncl_test_broker test_broker.c)
target_link_libraries(ncl_test_broker PRIVATE nclink_core)
add_test(NAME broker COMMAND ncl_test_broker)
set_tests_properties(broker PROPERTIES TIMEOUT 120)

# TLS transport: skipped unless the library was built with NCLINK_WITH_TLS=ON.
add_executable(ncl_test_tls test_tls.c)
target_link_libraries(ncl_test_tls PRIVATE nclink_core)
target_compile_definitions(ncl_test_tls PRIVATE
        NCL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME tls COMMAND ncl_test_tls)
set_tests_properties(tls PROPERTIES TIMEOUT 120)

# C++ wrapper: RAII, moves and exception mapping (header only, C++17).
if(NCLINK_BUILD_CPP)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_executable(ncl_test_cpp test_cpp.cpp)
    target_link_libraries(ncl_test_cpp PRIVATE nclink_core)
    add_test(NAME cpp COMMAND ncl_test_cpp)
endif()
