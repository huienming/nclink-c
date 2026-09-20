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

# The assembled device program: the FOCAS adapter is loaded as a module at run
# time and the point map comes from the configuration. Also covers the loader's
# failure paths (a foreign ABI, a file without the entry point).
if(NCLINK_BUILD_PLUGINS)
    add_executable(ncl_test_adapter_plugin test_adapter_plugin.c)
    target_link_libraries(ncl_test_adapter_plugin PRIVATE nclink_drivers)
    target_include_directories(ncl_test_adapter_plugin PRIVATE
            "${CMAKE_SOURCE_DIR}/tests")
    target_compile_definitions(ncl_test_adapter_plugin PRIVATE
            NCL_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:ncl_driver_focas>")
    # The refused fixtures sit in the same place as the real module.
    target_compile_definitions(ncl_test_adapter_plugin PRIVATE
            NCL_TEST_MODULE_DIR="$<TARGET_FILE_DIR:ncl_driver_test_bad_abi>")
    add_dependencies(ncl_test_adapter_plugin ncl_driver_focas)

    # Fixture modules the loader must refuse: a future ABI generation, and a
    # file that exports no entry point at all. They follow the same naming
    # convention as a real module (ncl_driver_<protocol>), so the test asks for
    # them by protocol name exactly as it asks for "focas".
    add_library(ncl_driver_test_bad_abi MODULE module_bad_abi.c)
    add_library(ncl_driver_test_no_entry MODULE module_no_entry.c)
    foreach(fixture ncl_driver_test_bad_abi ncl_driver_test_no_entry)
        set_target_properties(${fixture} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
        # The loader completes a protocol name with the platform's file name
        # ("libncl_test_bad_abi.so" on POSIX), so only MSVC drops the prefix.
        if(MSVC)
            set_target_properties(${fixture} PROPERTIES PREFIX "")
        endif()
        target_include_directories(${fixture} PRIVATE
                "${CMAKE_SOURCE_DIR}/include")
    endforeach()
    add_dependencies(ncl_test_adapter_plugin ncl_driver_test_bad_abi
            ncl_driver_test_no_entry)

    add_test(NAME adapter_plugin COMMAND ncl_test_adapter_plugin)

    # A one file adapter built the way an author builds one: the module declares
    # its points in code (ABI generation 2), so the point map never reaches the
    # configuration. It links only the core - that is all an adapter needs.
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

    add_executable(ncl_test_adapter_tool test_adapter_tool.c)
    target_link_libraries(ncl_test_adapter_tool PRIVATE nclink_drivers)
    target_include_directories(ncl_test_adapter_tool PRIVATE
            "${CMAKE_SOURCE_DIR}/tests")
    target_compile_definitions(ncl_test_adapter_tool PRIVATE
            NCL_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:ncl_driver_test_tool_basic>")
    add_dependencies(ncl_test_adapter_tool ncl_driver_test_tool_basic)
    add_test(NAME adapter_tool COMMAND ncl_test_adapter_tool)

    # A driver module (generation 1) for the tests that exercise the legacy
    # path: the FANUC adapter itself is a declaration now, so the fixtures keep
    # a driver module around to prove that path still works.
    add_library(ncl_driver_focas_fixture MODULE module_driver_focas.c
            "${CMAKE_SOURCE_DIR}/clients/focas/focas_driver.c"
            "${CMAKE_SOURCE_DIR}/clients/focas/focas_codec.c")
    target_link_libraries(ncl_driver_focas_fixture PRIVATE nclink_drivers)
    target_include_directories(ncl_driver_focas_fixture PRIVATE
            "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/clients")
    set_target_properties(ncl_driver_focas_fixture PROPERTIES
            OUTPUT_NAME "ncl_driver_focas_fixture"
            LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/plugins-driver-fixture"
            RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/plugins-driver-fixture")
    if(MSVC)
        set_target_properties(ncl_driver_focas_fixture PROPERTIES PREFIX "")
        target_compile_options(ncl_driver_focas_fixture PRIVATE /W4 /wd4996
                /utf-8)
        target_compile_definitions(ncl_driver_focas_fixture PRIVATE
                _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    else()
        target_compile_options(ncl_driver_focas_fixture PRIVATE -Wall -Wextra
                -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
    endif()
    target_compile_definitions(ncl_test_adapter_plugin PRIVATE
            NCL_TEST_DRIVER_FIXTURE_DIR="$<TARGET_FILE_DIR:ncl_driver_focas_fixture>")
    add_dependencies(ncl_test_adapter_plugin ncl_driver_focas_fixture)
endif()
add_executable(ncl_test_audit test_audit.c)
target_link_libraries(ncl_test_audit PRIVATE nclink_drivers)
target_include_directories(ncl_test_audit PRIVATE
        "${CMAKE_SOURCE_DIR}/tests")
add_test(NAME audit COMMAND ncl_test_audit)
