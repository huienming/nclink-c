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

add_executable(ncl_test_fins test_fins.c)
target_link_libraries(ncl_test_fins PRIVATE nclink_drivers)
target_include_directories(ncl_test_fins PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME fins COMMAND ncl_test_fins)

add_executable(ncl_test_s7 test_s7.c)
target_link_libraries(ncl_test_s7 PRIVATE nclink_drivers)
target_include_directories(ncl_test_s7 PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME s7 COMMAND ncl_test_s7)

add_executable(ncl_test_mtconnect test_mtconnect.c)
target_link_libraries(ncl_test_mtconnect PRIVATE nclink_drivers)
target_include_directories(ncl_test_mtconnect PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME mtconnect COMMAND ncl_test_mtconnect)

add_executable(ncl_test_meldas test_meldas.c)
target_link_libraries(ncl_test_meldas PRIVATE nclink_drivers)
target_include_directories(ncl_test_meldas PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME meldas COMMAND ncl_test_meldas)

add_executable(ncl_test_lsv2 test_lsv2.c)
target_link_libraries(ncl_test_lsv2 PRIVATE nclink_drivers)
target_include_directories(ncl_test_lsv2 PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME lsv2 COMMAND ncl_test_lsv2)

add_executable(ncl_test_syntec test_syntec.c)
target_link_libraries(ncl_test_syntec PRIVATE nclink_drivers)
target_include_directories(ncl_test_syntec PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME syntec COMMAND ncl_test_syntec)
add_executable(ncl_test_syntec_driver test_syntec_driver.c)
target_link_libraries(ncl_test_syntec_driver PRIVATE nclink_drivers)
target_include_directories(ncl_test_syntec_driver PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME syntec_driver COMMAND ncl_test_syntec_driver)

add_executable(ncl_test_focas test_focas.c)
target_link_libraries(ncl_test_focas PRIVATE nclink_drivers)
target_include_directories(ncl_test_focas PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME focas COMMAND ncl_test_focas)
add_executable(ncl_test_audit test_audit.c)
target_link_libraries(ncl_test_audit PRIVATE nclink_drivers)
target_include_directories(ncl_test_audit PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME audit COMMAND ncl_test_audit)
add_executable(ncl_test_knd test_knd.c)
target_link_libraries(ncl_test_knd PRIVATE nclink_drivers)
target_include_directories(ncl_test_knd PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME knd COMMAND ncl_test_knd)
