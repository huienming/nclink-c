# 单元测试：一个模块一个目录（core/ model/ message/ mqtt/ ...），共用的夹具与
# 工具头（ncl_test.h、fake_nclink_server.*、test_point_map.h、data/）在这一层。
# 适配器那一侧（模块声明 + 宿主端到端）的测试在 plugins/tests。

# 共用头：每个套件都要 #include "ncl_test.h"；测试用的数据（模型、TLS 证书）在 data/。
set(NCL_TEST_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")
set(NCL_TEST_DATA "${CMAKE_CURRENT_SOURCE_DIR}/data")

# ------------------------------------------------------------------ core ----
add_executable(ncl_test_json core/test_json.c)
target_link_libraries(ncl_test_json PRIVATE nclink_core)
target_include_directories(ncl_test_json PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME json COMMAND ncl_test_json)

add_executable(ncl_test_common core/test_common.c)
target_link_libraries(ncl_test_common PRIVATE nclink_core)
target_include_directories(ncl_test_common PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME common COMMAND ncl_test_common)

# 字符集：GB2312 → UTF-8（FANUC 的中文报警消息就是 GB2312，见 ncl_charset.h）。
add_executable(ncl_test_charset core/test_charset.c)
target_link_libraries(ncl_test_charset PRIVATE nclink_core)
target_include_directories(ncl_test_charset PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME charset COMMAND ncl_test_charset)

# Allocation seam: the same binary passes with the runtime allocator and with
# the NCL_STATIC_MEM pool (the pooling specific checks need NCL_STATIC_MEM).
add_executable(ncl_test_mem core/test_mem.c)
target_link_libraries(ncl_test_mem PRIVATE nclink_core)
target_include_directories(ncl_test_mem PRIVATE ${NCL_TEST_INCLUDES})
if(NCLINK_STATIC_MEM)
    target_compile_definitions(ncl_test_mem PRIVATE
            NCL_STATIC_MEM=1
            NCL_MEM_POOL_BYTES=${NCLINK_MEM_POOL_BYTES})
endif()
add_test(NAME mem COMMAND ncl_test_mem)

# Monte Carlo pressure test: random sizes and random operation sequences, with
# a replay of every seed to catch drift.
add_executable(ncl_test_mem_mc core/test_mem_mc.c)
target_link_libraries(ncl_test_mem_mc PRIVATE nclink_core)
target_include_directories(ncl_test_mem_mc PRIVATE ${NCL_TEST_INCLUDES})
if(NCLINK_STATIC_MEM)
    target_compile_definitions(ncl_test_mem_mc PRIVATE
            NCL_STATIC_MEM=1
            NCL_MEM_POOL_BYTES=${NCLINK_MEM_POOL_BYTES})
endif()
add_test(NAME mem_mc COMMAND ncl_test_mem_mc)
set_tests_properties(mem_mc PROPERTIES TIMEOUT 300)

# Multithreaded pressure test: several threads hammering one pool, handing
# ownership of blocks across threads.
add_executable(ncl_test_mem_mt core/test_mem_mt.c)
target_link_libraries(ncl_test_mem_mt PRIVATE nclink_core)
target_include_directories(ncl_test_mem_mt PRIVATE ${NCL_TEST_INCLUDES})
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

# 线程与计时（platform.c / thread_pool.c）。
add_executable(ncl_test_thread core/test_thread.c)
target_link_libraries(ncl_test_thread PRIVATE nclink_core)
target_include_directories(ncl_test_thread PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME thread COMMAND ncl_test_thread)

# Dynamic library loading: the fixture module next to the test binary stands in
# for a vendor adapter, so the loader is exercised against a real .dll/.so.
add_library(ncl_test_module MODULE core/test_module.c)
target_include_directories(ncl_test_module PRIVATE ${NCL_TEST_INCLUDES})
set_target_properties(ncl_test_module PROPERTIES
        PREFIX ""
        OUTPUT_NAME "ncl_test_module")
add_executable(ncl_test_library core/test_library.c)
target_link_libraries(ncl_test_library PRIVATE nclink_core)
target_include_directories(ncl_test_library PRIVATE ${NCL_TEST_INCLUDES})
target_compile_definitions(ncl_test_library PRIVATE
        NCL_TEST_MODULE_DIR="$<TARGET_FILE_DIR:ncl_test_module>")
add_dependencies(ncl_test_library ncl_test_module)
add_test(NAME library COMMAND ncl_test_library)

# --------------------------------------------------------------- general ----
add_executable(ncl_test_topic general/test_topic.c)
target_link_libraries(ncl_test_topic PRIVATE nclink_core)
target_include_directories(ncl_test_topic PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME topic COMMAND ncl_test_topic)

# ----------------------------------------------------------------- model ----
add_executable(ncl_test_model model/test_model.c)
target_link_libraries(ncl_test_model PRIVATE nclink_core)
target_include_directories(ncl_test_model PRIVATE ${NCL_TEST_INCLUDES})
target_compile_definitions(ncl_test_model PRIVATE NCL_TEST_DATA_DIR="${NCL_TEST_DATA}")
add_test(NAME model COMMAND ncl_test_model)

add_executable(ncl_test_message message/test_message.c)
target_link_libraries(ncl_test_message PRIVATE nclink_core)
target_include_directories(ncl_test_message PRIVATE ${NCL_TEST_INCLUDES})
target_compile_definitions(ncl_test_message PRIVATE NCL_TEST_DATA_DIR="${NCL_TEST_DATA}")
add_test(NAME message COMMAND ncl_test_message)

# ----------------------------------------------------------------- codec ----
add_executable(ncl_test_codec codec/test_codec.c)
target_link_libraries(ncl_test_codec PRIVATE nclink_core)
target_include_directories(ncl_test_codec PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME codec COMMAND ncl_test_codec)

# ------------------------------------------------------------------ mqtt ----
add_executable(ncl_test_mqtt mqtt/test_mqtt.c)
target_link_libraries(ncl_test_mqtt PRIVATE nclink_core)
target_include_directories(ncl_test_mqtt PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME mqtt COMMAND ncl_test_mqtt)

add_executable(ncl_test_mqtt_client mqtt/test_mqtt_client.c)
target_link_libraries(ncl_test_mqtt_client PRIVATE nclink_core)
target_include_directories(ncl_test_mqtt_client PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME mqtt_client COMMAND ncl_test_mqtt_client)
set_tests_properties(mqtt_client PROPERTIES TIMEOUT 60)

# TLS transport: skipped unless the library was built with NCLINK_WITH_TLS=ON.
add_executable(ncl_test_tls mqtt/test_tls.c)
target_link_libraries(ncl_test_tls PRIVATE nclink_core)
target_include_directories(ncl_test_tls PRIVATE ${NCL_TEST_INCLUDES})
target_compile_definitions(ncl_test_tls PRIVATE NCL_TEST_DATA_DIR="${NCL_TEST_DATA}")
add_test(NAME tls COMMAND ncl_test_tls)
set_tests_properties(tls PROPERTIES TIMEOUT 120)

# Interoperability against a real broker: skipped unless NCL_TEST_MQTT_BROKER
# is set (tools/interop.sh starts one in Docker and runs this suite).
add_executable(ncl_test_broker mqtt/test_broker.c)
target_link_libraries(ncl_test_broker PRIVATE nclink_core)
target_include_directories(ncl_test_broker PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME broker COMMAND ncl_test_broker)
set_tests_properties(broker PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------- client/server ---
# 两个套件都拿 fake_nclink_server.c（本目录下的假 broker/服务端）当靶子。
add_executable(ncl_test_client client/test_client.c fake_nclink_server.c)
target_link_libraries(ncl_test_client PRIVATE nclink_core)
target_include_directories(ncl_test_client PRIVATE ${NCL_TEST_INCLUDES})
target_compile_definitions(ncl_test_client PRIVATE NCL_TEST_DATA_DIR="${NCL_TEST_DATA}")
add_test(NAME client COMMAND ncl_test_client)
set_tests_properties(client PROPERTIES TIMEOUT 90)

add_executable(ncl_test_server server/test_server.c fake_nclink_server.c)
target_link_libraries(ncl_test_server PRIVATE nclink_core)
target_include_directories(ncl_test_server PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME server COMMAND ncl_test_server)
set_tests_properties(server PROPERTIES TIMEOUT 90)

add_executable(ncl_test_event server/test_event.c)
target_link_libraries(ncl_test_event PRIVATE nclink_core)
target_include_directories(ncl_test_event PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME event COMMAND ncl_test_event)
set_tests_properties(event PROPERTIES TIMEOUT 60)

# -------------------------------------------------------------- http/rest ---
add_executable(ncl_test_http http/test_http.c)
target_link_libraries(ncl_test_http PRIVATE nclink_core)
target_include_directories(ncl_test_http PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME http COMMAND ncl_test_http)
set_tests_properties(http PROPERTIES TIMEOUT 60)

add_executable(ncl_test_rest rest/test_rest.c)
target_link_libraries(ncl_test_rest PRIVATE nclink_core)
target_include_directories(ncl_test_rest PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME rest COMMAND ncl_test_rest)
set_tests_properties(rest PROPERTIES TIMEOUT 60)

# ----------------------------------------------------------------- config ---
add_executable(ncl_test_config config/test_config.c)
target_link_libraries(ncl_test_config PRIVATE nclink_core)
target_include_directories(ncl_test_config PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME config COMMAND ncl_test_config)
set_tests_properties(config PROPERTIES TIMEOUT 60)

# -------------------------------------------------------------------- ftp ---
add_executable(ncl_test_ftp ftp/test_ftp.c)
target_link_libraries(ncl_test_ftp PRIVATE nclink_core)
target_include_directories(ncl_test_ftp PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME ftp COMMAND ncl_test_ftp)
set_tests_properties(ftp PROPERTIES TIMEOUT 120)

# ------------------------------------------------------------------- file ---
add_executable(ncl_test_file file/test_file.c)
target_link_libraries(ncl_test_file PRIVATE nclink_core)
target_include_directories(ncl_test_file PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME file COMMAND ncl_test_file)
set_tests_properties(file PROPERTIES TIMEOUT 180)

# ----------------------------------------------------------------- schema ---
add_executable(ncl_test_schema schema/test_schema.c)
target_link_libraries(ncl_test_schema PRIVATE nclink_core)
target_include_directories(ncl_test_schema PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME schema COMMAND ncl_test_schema)
set_tests_properties(schema PROPERTIES TIMEOUT 60)

# ---------------------------------------------------------------- license ---
# 仓库级检查：每个源文件头部的 SPDX 行（NCL_TEST_SOURCE_ROOT = 仓库根）。
add_executable(ncl_test_license license/test_license.c)
target_link_libraries(ncl_test_license PRIVATE nclink_core)
target_include_directories(ncl_test_license PRIVATE ${NCL_TEST_INCLUDES})
target_compile_definitions(ncl_test_license PRIVATE
        NCL_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME license COMMAND ncl_test_license)

# ------------------------------------------------------------ tool layer ---
# 驱动骨架（地址解析、错误分级、read_one/write_one 的会话规则）、审计、绑定档都
# 在核心库里；这两个套件用 "mock" 协议实现来驱动骨架，所以还要连 nclink_clients
# （-DNCLINK_BUILD_CLIENTS=OFF 时跳过）。
#
# 这里用 NCLINK_BUILD_CLIENTS 而不是 if(TARGET nclink_clients)：根 CMakeLists 先
# add_subdirectory(stack/test) 再 add_subdirectory(clients)，此刻那个目标还不存在。
if(NCLINK_BUILD_CLIENTS)
    add_executable(ncl_test_driver tool/test_driver.c)
    target_link_libraries(ncl_test_driver PRIVATE nclink_core nclink_clients)
    target_include_directories(ncl_test_driver PRIVATE ${NCL_TEST_INCLUDES})
    add_test(NAME driver COMMAND ncl_test_driver)

    add_executable(ncl_test_audit tool/test_audit.c)
    target_link_libraries(ncl_test_audit PRIVATE nclink_core nclink_clients)
    target_include_directories(ncl_test_audit PRIVATE ${NCL_TEST_INCLUDES})
    add_test(NAME audit COMMAND ncl_test_audit)

    # 绑定档：client 的语义函数直接绑到模型路径上（不需要 author 写 dispatch）
    add_executable(ncl_test_bind tool/test_bind.c)
    target_link_libraries(ncl_test_bind PRIVATE nclink_core)
    target_include_directories(ncl_test_bind PRIVATE ${NCL_TEST_INCLUDES})
    add_test(NAME bind COMMAND ncl_test_bind)
endif()

# 适配器作者的声明契约（端到端的模块装载在 plugins/tests）。
add_executable(ncl_test_tool tool/test_tool.c)
target_link_libraries(ncl_test_tool PRIVATE nclink_core)
target_include_directories(ncl_test_tool PRIVATE ${NCL_TEST_INCLUDES})
add_test(NAME tool COMMAND ncl_test_tool)

# C++ wrapper: RAII, moves and exception mapping (header only, C++17).
if(NCLINK_BUILD_CPP)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_executable(ncl_test_cpp cpp/test_cpp.cpp)
    target_link_libraries(ncl_test_cpp PRIVATE nclink_core)
    target_include_directories(ncl_test_cpp PRIVATE ${NCL_TEST_INCLUDES})
    add_test(NAME cpp COMMAND ncl_test_cpp)
endif()
