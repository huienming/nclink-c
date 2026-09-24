# 示例程序：默认随库一起编译，只依赖 nclink_core。
#
# 目录按“哪一侧”分：examples/device/c 是设备端（含五个语言共用的设备模型），
# examples/client/c 是客户端；C++ 版（examples/client/cpp、examples/device/cpp）
# 只作为源码发布，各语言绑定的示例在 examples/sdk 与 examples/{client,device}。

# 设备模型编译进示例：device_model.c 是五个语言设备端示例共用的唯一出处，
# 分发时不依赖任何外部模型文件。
set(NCL_DEVICE_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/device/c/device_model.c")

add_executable(ncl_device_demo device/c/ncl_device_demo.c ${NCL_DEVICE_MODEL})
target_link_libraries(ncl_device_demo PRIVATE nclink_core)

add_executable(ncl_client_demo client/c/ncl_client_demo.c)
target_link_libraries(ncl_client_demo PRIVATE nclink_core)

# 文件通道吞吐 / 跨主机传输测试台（设备端与客户端两个角色，见文件头注释）
add_executable(ncl_file_bench device/c/ncl_file_bench.c ${NCL_DEVICE_MODEL})
target_link_libraries(ncl_file_bench PRIVATE nclink_core)
