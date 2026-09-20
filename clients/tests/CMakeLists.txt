# Protocol client tests: golden frames (byte level diffs) and runs against a
# mock peer.
#
# They drive a client through the driver registry (ncl_driver_create), which
# lives in the adapter layer, so they link nclink_drivers - that brings
# nclink_clients in transitively. Once an adapter talks to a client directly
# (no registry), these link nclink::clients alone.

set(NCLINK_CLIENT_TESTS modbus mc fins s7 mtconnect meldas lsv2 syntec knd focas)

foreach(name IN LISTS NCLINK_CLIENT_TESTS)
    add_executable(ncl_test_${name} test_${name}.c)
    target_link_libraries(ncl_test_${name} PRIVATE nclink_drivers)
    target_include_directories(ncl_test_${name} PRIVATE
            "${CMAKE_SOURCE_DIR}/tests")
    add_test(NAME ${name} COMMAND ncl_test_${name})
endforeach()

# The syntec client has a second, vector level suite.
add_executable(ncl_test_syntec_driver test_syntec_driver.c)
target_link_libraries(ncl_test_syntec_driver PRIVATE nclink_drivers)
target_include_directories(ncl_test_syntec_driver PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME syntec_driver COMMAND ncl_test_syntec_driver)
