add_executable(ncl_test_driver test_driver.c)
target_link_libraries(ncl_test_driver PRIVATE nclink_drivers)
target_include_directories(ncl_test_driver PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME driver COMMAND ncl_test_driver)
