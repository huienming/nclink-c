# 示例程序：默认随库一起编译，只依赖 nclink_core。

add_executable(ncl_device_demo ncl_device_demo.c)
target_link_libraries(ncl_device_demo PRIVATE nclink_core)

add_executable(ncl_client_demo ncl_client_demo.c)
target_link_libraries(ncl_client_demo PRIVATE nclink_core)
