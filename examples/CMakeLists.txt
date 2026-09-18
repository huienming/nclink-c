# 示例程序：默认随库一起编译，只依赖 nclink_core。

# 设备模型编译进示例：device_model.c 是五个语言设备端示例共用的唯一出处，
# 分发时不依赖任何外部模型文件。
add_executable(ncl_device_demo ncl_device_demo.c device_model.c)
target_link_libraries(ncl_device_demo PRIVATE nclink_core)

add_executable(ncl_client_demo ncl_client_demo.c)
target_link_libraries(ncl_client_demo PRIVATE nclink_core)

# 文件通道吞吐 / 跨主机传输测试台（设备端与客户端两个角色，见文件头注释）
add_executable(ncl_file_bench ncl_file_bench.c device_model.c)
target_link_libraries(ncl_file_bench PRIVATE nclink_core)
