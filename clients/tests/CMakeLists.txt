# Protocol client tests: golden frames (byte level diffs) and runs against a
# mock peer.
#
# 每个套件自己构造它测的那个客户端（ncl_modbus_tcp_create() 之类），再把它交给
# tests/test_point_map.h 这座桥去跑地址读写；骨架（ncl_driver_read_one）在核心库
# 里，nclink_clients 已经公开依赖核心库，所以这里连 nclink_clients 就够了。

set(NCLINK_CLIENT_TESTS modbus mc fins s7 mtconnect meldas lsv2 syntec knd focas)

foreach(name IN LISTS NCLINK_CLIENT_TESTS)
    add_executable(ncl_test_${name} test_${name}.c)
    target_link_libraries(ncl_test_${name} PRIVATE nclink_clients)
    target_include_directories(ncl_test_${name} PRIVATE
            "${CMAKE_SOURCE_DIR}/stack/test")
    add_test(NAME ${name} COMMAND ncl_test_${name})
endforeach()

# The syntec client has a second, vector level suite.
add_executable(ncl_test_syntec_driver test_syntec_driver.c)
target_link_libraries(ncl_test_syntec_driver PRIVATE nclink_clients)
target_include_directories(ncl_test_syntec_driver PRIVATE
        "${CMAKE_SOURCE_DIR}/stack/test")
add_test(NAME syntec_driver COMMAND ncl_test_syntec_driver)
