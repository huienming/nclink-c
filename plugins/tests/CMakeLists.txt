# 适配器这一侧的测试：夹具模块与"声明 → 模型 → 绑定 → 请求应答"的端到端。
#
# 夹具按作者的写法编译（一个文件一个适配器，只连核心库），装载器必须拒收的两个
# （外来 ABI、根本没有入口）也在这一层。
if(NOT NCLINK_BUILD_TESTS)
    return()
endif()

add_library(ncl_driver_test_tool_basic MODULE module_tool_basic.c)
target_link_libraries(ncl_driver_test_tool_basic PRIVATE nclink_core)
target_include_directories(ncl_driver_test_tool_basic PRIVATE
        "${CMAKE_SOURCE_DIR}/stack/include")
set_target_properties(ncl_driver_test_tool_basic PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins-tool-fixture"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins-tool-fixture")
if(MSVC)
    set_target_properties(ncl_driver_test_tool_basic PROPERTIES PREFIX "")
    target_compile_options(ncl_driver_test_tool_basic PRIVATE /W4 /wd4996 /utf-8)
    target_compile_definitions(ncl_driver_test_tool_basic PRIVATE
            _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    target_compile_options(ncl_driver_test_tool_basic PRIVATE -Wall -Wextra
            -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
endif()

# 装载器必须拒收的两个夹具：外来 ABI（含旧式"交驱动工厂"的模块），以及根本
# 不导出入口的文件。
add_library(ncl_driver_test_bad_abi MODULE module_bad_abi.c)
add_library(ncl_driver_test_no_entry MODULE module_no_entry.c)
foreach(fixture ncl_driver_test_bad_abi ncl_driver_test_no_entry)
    set_target_properties(${fixture} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/plugins-refused-fixture"
            RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/plugins-refused-fixture")
    # 装载器会用平台的文件名补全模块名（POSIX 上是 libncl_*.so），所以只有
    # MSVC 需要去掉前缀。
    if(MSVC)
        set_target_properties(${fixture} PROPERTIES PREFIX "")
    endif()
    target_include_directories(${fixture} PRIVATE
            "${CMAKE_SOURCE_DIR}/stack/include")
endforeach()

# 宿主 + 模块装载的端到端：一个模块声明点位，宿主把它变成模型、绑定与请求应答；
# 同一个套件覆盖装载器的失败路径。
add_executable(ncl_test_host_tool test_host_tool.c)
target_link_libraries(ncl_test_host_tool PRIVATE nclink_core)
target_include_directories(ncl_test_host_tool PRIVATE "${CMAKE_SOURCE_DIR}/stack/test")
target_compile_definitions(ncl_test_host_tool PRIVATE
        NCL_TEST_PLUGIN_DIR="$<TARGET_FILE_DIR:ncl_driver_test_tool_basic>"
        NCL_TEST_MODULE_DIR="$<TARGET_FILE_DIR:ncl_driver_test_bad_abi>"
        # 伪机床是 plugins/ 里的真适配器（不是夹具）：同一套用例顺带把它按现场那套
        # 走一遍 —— 装载、建模、取值、写回、四张表、遥控。
        NCL_TEST_PSEUDO_DIR="$<TARGET_FILE_DIR:ncl_driver_pseudo>")
add_dependencies(ncl_test_host_tool ncl_driver_test_tool_basic
        ncl_driver_test_bad_abi ncl_driver_test_no_entry ncl_driver_pseudo)
add_test(NAME host_tool COMMAND ncl_test_host_tool)
