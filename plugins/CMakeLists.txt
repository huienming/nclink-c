# 厂商适配器（源码）：一个文件一个适配器。
#
#   plugins/focas.c  ->  <build>/plugins/ncl_driver_focas.dll (libncl_driver_focas.so)
#
# 命名约定：ncl_driver_<工具名>.<后缀>，所以配置里写工具名（"focas"）就够了，
# 装载器自己补文件名。这里没有“注册表”：模块只导出一个入口
# ncl_adapter_module()，它返回声明好的点位（见 include/nclink/ncl_tool.h）。
#
# 关掉插件（-DNCLINK_BUILD_PLUGINS=OFF）得到的是一个不装载任何厂商适配器的
# 设备程序：照样起来、照样提供 REST 与文件工具，只是没有点位。

file(GLOB NCLINK_PLUGIN_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

# 适配器用协议客户端去跟机床说话（ncl_focas_create() 之类），所以模块连
# nclink_clients；核心库本身不认识这些协议。
set(NCLINK_PLUGIN_LIBS nclink_core)
if(TARGET nclink_clients)
    list(APPEND NCLINK_PLUGIN_LIBS nclink_clients)
endif()

foreach(source IN LISTS NCLINK_PLUGIN_SOURCES)
    get_filename_component(name "${source}" NAME_WE)
    set(target "ncl_driver_${name}")
    add_library(${target} MODULE "${source}")
    target_link_libraries(${target} PRIVATE ${NCLINK_PLUGIN_LIBS})
    target_include_directories(${target} PRIVATE
            "${CMAKE_SOURCE_DIR}/stack/include")
    set_target_properties(${target} PROPERTIES
            OUTPUT_NAME "ncl_driver_${name}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /wd4996 /utf-8)
        target_compile_definitions(${target} PRIVATE
                _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow
                -Wstrict-prototypes -Wmissing-prototypes)
    endif()

    # A protocol client's own test suite can double as the adapter's integration
    # test: it loads this module by protocol name, builds the device with the
    # host and reads the points off its mock controller (see
    # clients/tests/test_syntec_driver.c). The test lives in clients/tests, which
    # is configured before this directory, so the wiring happens here - where
    # both the module and the test target are known.
    if(NCLINK_BUILD_TESTS AND TARGET ncl_test_${name}_driver)
        string(TOUPPER "${name}" name_upper)
        target_compile_definitions(ncl_test_${name}_driver PRIVATE
                "NCL_${name_upper}_PLUGIN_DIR=\"$<TARGET_FILE_DIR:${target}>\"")
        add_dependencies(ncl_test_${name}_driver ${target})
    endif()
endforeach()

message(STATUS "nclink: adapter modules -> ${CMAKE_BINARY_DIR}/plugins")

# 适配器这一侧的测试（夹具模块 + 宿主端到端）。
add_subdirectory(tests)
