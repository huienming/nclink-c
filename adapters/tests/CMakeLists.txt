add_executable(ncl_test_driver test_driver.c)
target_link_libraries(ncl_test_driver PRIVATE nclink_drivers)
target_include_directories(ncl_test_driver PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME driver COMMAND ncl_test_driver)

add_executable(ncl_test_driver_manager test_driver_manager.c)
target_link_libraries(ncl_test_driver_manager PRIVATE nclink_drivers)
target_include_directories(ncl_test_driver_manager PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME driver_manager COMMAND ncl_test_driver_manager)

add_executable(ncl_test_adapter test_adapter.c)
target_link_libraries(ncl_test_adapter PRIVATE nclink_drivers)
target_include_directories(ncl_test_adapter PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME adapter COMMAND ncl_test_adapter)

add_executable(ncl_test_modbus test_modbus.c)
target_link_libraries(ncl_test_modbus PRIVATE nclink_drivers)
target_include_directories(ncl_test_modbus PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME modbus COMMAND ncl_test_modbus)

add_executable(ncl_test_mc test_mc.c)
target_link_libraries(ncl_test_mc PRIVATE nclink_drivers)
target_include_directories(ncl_test_mc PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME mc COMMAND ncl_test_mc)
