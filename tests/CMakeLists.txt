enable_testing()

# Find Catch2 or use FetchContent
find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.4.0
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# Test executable
add_executable(elcache_tests
    test_main.cpp
    test_types.cpp
    test_chunk.cpp
    test_arc_cache.cpp
    test_memory_cache.cpp
    test_disk_cache.cpp
    test_coordinator.cpp
    test_cluster.cpp
    test_protocol.cpp
    test_metrics.cpp
)

target_link_libraries(elcache_tests PRIVATE
    elcache
    Catch2::Catch2WithMain
)

target_include_directories(elcache_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(elcache_tests)

# SDK tests (C tests)
add_executable(sdk_tests
    test_sdk.c
)

target_link_libraries(sdk_tests PRIVATE
    elcache_sdk
)

target_include_directories(sdk_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

add_test(NAME sdk_tests COMMAND sdk_tests)
