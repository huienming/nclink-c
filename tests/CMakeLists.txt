add_executable(ncl_test_json test_json.c)
target_link_libraries(ncl_test_json PRIVATE nclink_core)
add_test(NAME json COMMAND ncl_test_json)

add_executable(ncl_test_common test_common.c)
target_link_libraries(ncl_test_common PRIVATE nclink_core)
add_test(NAME common COMMAND ncl_test_common)

add_executable(ncl_test_topic test_topic.c)
target_link_libraries(ncl_test_topic PRIVATE nclink_core)
add_test(NAME topic COMMAND ncl_test_topic)

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
