# Examples

add_executable(basic_usage basic_usage.cpp)
target_link_libraries(basic_usage PRIVATE elcache)

add_executable(sdk_example sdk_example.c)
target_link_libraries(sdk_example PRIVATE elcache_sdk)
