add_executable(ncl_test_driver test_driver.c)
target_link_libraries(ncl_test_driver PRIVATE nclink_drivers)
target_include_directories(ncl_test_driver PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME driver COMMAND ncl_test_driver)

# The assembled device program: one module declares the points, the host builds
# the model and the bindings from that declaration, and a real Query/Set/Method
# goes through it. Also covers the loader's failure paths (a foreign ABI, a file
# without the entry point).
if(NCLINK_BUILD_PLUGINS)
    # A one file adapter, built the way an author builds one. It links only the
    # core - that is all an adapter needs.
    add_library(ncl_driver_test_tool_basic MODULE module_tool_basic.c)
    target_link_libraries(ncl_driver_test_tool_basic PRIVATE nclink_core)
    target_include_directories(ncl_driver_test_tool_basic PRIVATE
            "${CMAKE_SOURCE_DIR}/include")
    set_target_properties(ncl_driver_test_tool_basic PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins-tool-fixture"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins-tool-fixture")
    if(MSVC)
        set_target_properties(ncl_driver_test_tool_basic PROPERTIES PREFIX "")
        target_compile_options(ncl_driver_test_tool_basic PRIVATE /W4 /wd4996
                /utf-8)
        target_compile_definitions(ncl_driver_test_tool_basic PRIVATE
                _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    else()
        target_compile_options(ncl_driver_test_tool_basic PRIVATE -Wall -Wextra
                -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
    endif()

    # Fixture modules the loader must refuse: a foreign ABI generation (which
    # includes the old driver modules, generation 1), and a file that exports no
    # entry point at all.
    add_library(ncl_driver_test_bad_abi MODULE module_bad_abi.c)
    add_library(ncl_driver_test_no_entry MODULE module_no_entry.c)
    foreach(fixture ncl_driver_test_bad_abi ncl_driver_test_no_entry)
        set_target_properties(${fixture} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/plugins-refused-fixture"
                RUNTIME_OUTPUT_DIRECTORY
                "${CMAKE_BINARY_DIR}/plugins-refused-fixture")
        # The loader completes a protocol name with the platform's file name
        # ("libncl_test_bad_abi.so" on POSIX), so only MSVC drops the prefix.
        if(MSVC)
            set_target_properties(${fixture} PROPERTIES PREFIX "")
        endif()
        target_include_directories(${fixture} PRIVATE
                "${CMAKE_SOURCE_DIR}/include")
    endforeach()

    add_executable(ncl_test_adapter_tool test_adapter_tool.c)
    target_link_libraries(ncl_test_adapter_tool PRIVATE nclink_drivers)
    target_include_directories(ncl_test_adapter_tool PRIVATE
            "${CMAKE_SOURCE_DIR}/tests")
    target_compile_definitions(ncl_test_adapter_tool PRIVATE
            NCL_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:ncl_driver_test_tool_basic>"
            NCL_TEST_MODULE_DIR="$<TARGET_FILE_DIR:ncl_driver_test_bad_abi>")
    add_dependencies(ncl_test_adapter_tool ncl_driver_test_tool_basic
            ncl_driver_test_bad_abi ncl_driver_test_no_entry)
    add_test(NAME adapter_tool COMMAND ncl_test_adapter_tool)
endif()

add_executable(ncl_test_audit test_audit.c)
target_link_libraries(ncl_test_audit PRIVATE nclink_drivers)
target_include_directories(ncl_test_audit PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME audit COMMAND ncl_test_audit)
