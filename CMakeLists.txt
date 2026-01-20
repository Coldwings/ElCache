cmake_minimum_required(VERSION 3.20)
project(ElCache VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(ELCACHE_BUILD_TESTS "Build tests" ON)
option(ELCACHE_BUILD_EXAMPLES "Build examples" ON)
option(ELCACHE_BUILD_SDK "Build C SDK" ON)
option(ELCACHE_BUILD_DOCS "Build documentation" OFF)

include(FetchContent)

# =============================================================================
# Dependencies via FetchContent
# =============================================================================

# Elio - C++20 coroutine library
FetchContent_Declare(
    elio
    GIT_REPOSITORY https://github.com/Coldwings/Elio.git
    GIT_TAG        main
)

# xxHash - Fast hashing
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
)

# fmt - Formatting library (required by Elio)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
)

message(STATUS "Fetching dependencies...")

# Fetch fmt first (Elio depends on it)
FetchContent_MakeAvailable(fmt)

# Fetch Elio
FetchContent_GetProperties(elio)
if(NOT elio_POPULATED)
    FetchContent_Populate(elio)
    # Elio is header-only, just need include path
    set(ELIO_INCLUDE_DIR ${elio_SOURCE_DIR}/include)
endif()

# Fetch xxHash
FetchContent_GetProperties(xxhash)
if(NOT xxhash_POPULATED)
    FetchContent_Populate(xxhash)
    # Build xxHash as a library
    add_library(xxhash STATIC ${xxhash_SOURCE_DIR}/xxhash.c)
    target_include_directories(xxhash PUBLIC ${xxhash_SOURCE_DIR})
    set(XXHASH_INCLUDE_DIR ${xxhash_SOURCE_DIR})
endif()

message(STATUS "Found Elio at: ${ELIO_INCLUDE_DIR}")
message(STATUS "Found xxHash at: ${XXHASH_INCLUDE_DIR}")

# =============================================================================
# System dependencies
# =============================================================================

# OpenSSL for TLS support
find_package(OpenSSL REQUIRED)

# liburing (optional, for io_uring support)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBURING IMPORTED_TARGET liburing)
endif()

# =============================================================================
# Main library
# =============================================================================

add_library(elcache
    src/core/types.cpp
    src/core/chunk.cpp
    src/core/config.cpp
    src/cache/arc_cache.cpp
    src/cache/memory_cache.cpp
    src/cache/disk_cache.cpp
    src/cache/cache_coordinator.cpp
    src/cache/sparse_cache.cpp
    src/storage/chunk_store.cpp
    src/storage/disk_store.cpp
    src/network/node.cpp
    src/network/gossip.cpp
    src/network/cluster.cpp
    src/network/unix_server.cpp
    src/protocol/protocol.cpp
    src/protocol/codec.cpp
    src/http/http_api.cpp
    src/http/metrics.cpp
    src/util/hash.cpp
    src/util/lru.cpp
)

target_include_directories(elcache PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${ELIO_INCLUDE_DIR}
    ${XXHASH_INCLUDE_DIR}
)

target_link_libraries(elcache PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    fmt::fmt
    xxhash
    pthread
)

target_compile_definitions(elcache PUBLIC ELCACHE_USE_XXHASH=1)

if(LIBURING_FOUND)
    target_link_libraries(elcache PUBLIC PkgConfig::LIBURING)
    target_compile_definitions(elcache PUBLIC ELCACHE_USE_IOURING=1)
    message(STATUS "io_uring support: enabled")
else()
    message(STATUS "io_uring support: disabled (liburing not found)")
endif()

# =============================================================================
# Main executable
# =============================================================================

add_executable(elcached src/main.cpp)
target_link_libraries(elcached PRIVATE elcache)

# =============================================================================
# C SDK (zero dependencies)
# =============================================================================

if(ELCACHE_BUILD_SDK)
    add_library(elcache_sdk SHARED
        src/sdk/elcache_client.c
        src/sdk/shm_transport.c
        src/sdk/unix_transport.c
        src/sdk/protocol.c
    )
    
    target_include_directories(elcache_sdk PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    
    set_target_properties(elcache_sdk PROPERTIES
        C_STANDARD 11
        PUBLIC_HEADER "include/elcache/elcache_sdk.h"
        VERSION ${PROJECT_VERSION}
        SOVERSION 0
    )
    
    # SDK has no external dependencies - only POSIX
    target_link_libraries(elcache_sdk PRIVATE pthread rt)
    
    # Also build static version
    add_library(elcache_sdk_static STATIC
        src/sdk/elcache_client.c
        src/sdk/shm_transport.c
        src/sdk/unix_transport.c
        src/sdk/protocol.c
    )
    
    target_include_directories(elcache_sdk_static PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    
    set_target_properties(elcache_sdk_static PROPERTIES
        C_STANDARD 11
        OUTPUT_NAME elcache_sdk
    )
endif()

# =============================================================================
# Tests
# =============================================================================

if(ELCACHE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# =============================================================================
# Examples
# =============================================================================

if(ELCACHE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# =============================================================================
# Documentation
# =============================================================================

if(ELCACHE_BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
        set(DOXYGEN_GENERATE_HTML YES)
        set(DOXYGEN_GENERATE_MAN NO)
        set(DOXYGEN_EXTRACT_ALL YES)
        set(DOXYGEN_EXTRACT_PRIVATE NO)
        set(DOXYGEN_EXTRACT_STATIC YES)
        
        doxygen_add_docs(docs
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/docs
            COMMENT "Generating API documentation"
        )
    endif()
endif()

# =============================================================================
# Install
# =============================================================================

include(GNUInstallDirs)

install(TARGETS elcache elcached
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/elcache 
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    OPTIONAL
)

install(FILES config/elcache.json.example
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/elcache
    RENAME elcache.json.example
)

if(ELCACHE_BUILD_SDK)
    install(TARGETS elcache_sdk elcache_sdk_static
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/elcache
    )
endif()

# =============================================================================
# Package configuration
# =============================================================================

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ElCacheConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# =============================================================================
# Summary
# =============================================================================

message(STATUS "")
message(STATUS "ElCache ${PROJECT_VERSION} configuration:")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build tests:    ${ELCACHE_BUILD_TESTS}")
message(STATUS "  Build examples: ${ELCACHE_BUILD_EXAMPLES}")
message(STATUS "  Build SDK:      ${ELCACHE_BUILD_SDK}")
message(STATUS "  Build docs:     ${ELCACHE_BUILD_DOCS}")
message(STATUS "")
