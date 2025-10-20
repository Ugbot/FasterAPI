cmake_minimum_required(VERSION 3.20)
project(fasterapi LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -----------------------------
# Feature Flags
# -----------------------------
option(FA_BUILD_PG           "Build PostgreSQL integration" ON)
option(FA_BUILD_HTTP         "Build HTTP server" ON)
option(FA_USE_LIBUV          "Use libuv for event loop (required on Windows)" ON)
option(FA_USE_ASIO           "Build with standalone Asio (for Beast/H2 path)" OFF)
option(FA_ENABLE_HTTP2       "Enable HTTP/2 support via nghttp2" ON)
option(FA_ENABLE_HTTP3       "Enable HTTP/3 support via MsQuic" OFF)
option(FA_ENABLE_COMPRESSION "Enable zstd compression" ON)
option(FA_ENABLE_MIMALLOC    "Link mimalloc as default allocator" ON)
option(FA_USE_UWEBSOCKETS    "Use uWebSockets for HTTP/1.1 and WebSocket" OFF)
option(FA_SANITIZE           "Enable Address/UB sanitizers (non-MSVC)" OFF)

# Optimization flags for maximum performance
# Use -mcpu=native on Apple, -march=native on other platforms
if(APPLE)
    set(NATIVE_FLAG "-mcpu=native")
else()
    set(NATIVE_FLAG "-march=native")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 ${NATIVE_FLAG} -flto -fno-exceptions -fno-rtti")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fno-exceptions -fno-rtti")

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX Flags: ${CMAKE_CXX_FLAGS_RELEASE}")
message(STATUS "Building PostgreSQL: ${FA_BUILD_PG}")
message(STATUS "Building HTTP server: ${FA_BUILD_HTTP}")
message(STATUS "HTTP/2 support: ${FA_ENABLE_HTTP2}")
message(STATUS "HTTP/3 support: ${FA_ENABLE_HTTP3}")
message(STATUS "Compression: ${FA_ENABLE_COMPRESSION}")

# -----------------------------
# Dependency Manager (CPM)
# -----------------------------
include(cmake/CPM.cmake)

# Platform-specific settings
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
    message(STATUS "Building for macOS architectures: ${CMAKE_OSX_ARCHITECTURES}")
    set(LIB_EXTENSION dylib)
elseif(LINUX)
    set(LIB_EXTENSION so)
elseif(WIN32)
    set(LIB_EXTENSION dll)
else()
    set(LIB_EXTENSION so)
endif()

# -----------------------------
# Shared Dependencies
# -----------------------------

# Core library sources (reactor, futures, tasks, buffers)
set(CORE_SOURCES
    src/cpp/core/reactor.cpp
    src/cpp/core/ring_buffer.cpp
    src/cpp/core/async_io.cpp
)

# Platform-specific async I/O implementations
if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    list(APPEND CORE_SOURCES src/cpp/core/async_io_kqueue.cpp)
    message(STATUS "Async I/O backend: kqueue (macOS/BSD)")
elseif(UNIX)
    # Linux - epoll and optionally io_uring
    list(APPEND CORE_SOURCES src/cpp/core/async_io_epoll.cpp)
    message(STATUS "Async I/O backend: epoll (Linux)")
    
    # Check for io_uring support (optional)
    find_library(LIBURING_LIB uring)
    if(LIBURING_LIB)
        option(FA_ENABLE_IO_URING "Enable io_uring support (Linux 5.1+)" ON)
        if(FA_ENABLE_IO_URING)
            list(APPEND CORE_SOURCES src/cpp/core/async_io_uring.cpp)
            add_compile_definitions(HAVE_LIBURING)
            target_link_libraries(fasterapi_http PRIVATE ${LIBURING_LIB})
            message(STATUS "io_uring support: ENABLED")
        endif()
    else()
        message(STATUS "io_uring support: DISABLED (liburing not found)")
    endif()
elseif(WIN32)
    list(APPEND CORE_SOURCES src/cpp/core/async_io_iocp.cpp)
    message(STATUS "Async I/O backend: IOCP (Windows)")
endif()

set(CORE_HEADERS
    src/cpp/core/reactor.h
    src/cpp/core/future.h
    src/cpp/core/task.h
    src/cpp/core/ring_buffer.h
    src/cpp/core/async_io.h
)

# Python executor sources
set(PYTHON_SOURCES
    src/cpp/python/gil_guard.cpp
    src/cpp/python/py_executor.cpp
)

set(PYTHON_HEADERS
    src/cpp/python/gil_guard.h
    src/cpp/python/py_executor.h
)

# Native types (NumPy-style for web)
set(NATIVE_TYPES_SOURCES
    src/cpp/types/native_value.cpp
    src/cpp/types/native_request.cpp
)

set(NATIVE_TYPES_HEADERS
    src/cpp/types/native_value.h
    src/cpp/types/native_request.h
)

# Python (required for executor)
find_package(Python3 COMPONENTS Development REQUIRED)
message(STATUS "Python found: ${Python3_VERSION}")

# OpenSSL (required for TLS, ALPN)
if (FA_ENABLE_HTTP2 OR FA_ENABLE_HTTP3)
    find_package(OpenSSL REQUIRED)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
endif()

# mimalloc (allocator override)
if (FA_ENABLE_MIMALLOC)
    CPMAddPackage(
        NAME mimalloc
        GITHUB_REPOSITORY microsoft/mimalloc
        GIT_TAG v2.1.7
        OPTIONS
            "MI_BUILD_TESTS OFF"
            "MI_BUILD_SHARED ON"
            "MI_BUILD_STATIC ON"
    )
endif()

# -----------------------------
# HTTP Server Dependencies
# -----------------------------

if (FA_BUILD_HTTP)
    # libuv (event loop - required on Windows)
    if (FA_USE_LIBUV)
        CPMAddPackage(
            NAME libuv
            GITHUB_REPOSITORY libuv/libuv
            GIT_TAG v1.49.2
            OPTIONS "LIBUV_BUILD_TESTS OFF"
        )
    endif()

    # uWebSockets (HTTP/1.1 + WebSocket) - has its own HTTP parser
    if (FA_USE_UWEBSOCKETS)
        CPMAddPackage(
            NAME uwebsockets
            GITHUB_REPOSITORY uNetworking/uWebSockets
            GIT_TAG v20.51.0
        )
    endif()

    # simdjson (fast JSON parsing)
    CPMAddPackage(
        NAME simdjson
        GITHUB_REPOSITORY simdjson/simdjson
        GIT_TAG v3.10.1
    )

    # zstd (compression)
    if (FA_ENABLE_COMPRESSION)
        CPMAddPackage(
            NAME zstd
            GITHUB_REPOSITORY facebook/zstd
            GIT_TAG v1.5.6
            OPTIONS
                "ZSTD_BUILD_PROGRAMS OFF"
                "ZSTD_BUILD_TESTS OFF"
        )
    endif()

    # nghttp2 (HTTP/2)
    if (FA_ENABLE_HTTP2)
        CPMAddPackage(
            NAME nghttp2
            GITHUB_REPOSITORY nghttp2/nghttp2
            GIT_TAG v1.63.0
            OPTIONS
                "ENABLE_EXAMPLES OFF"
                "ENABLE_HPACK_TOOLS OFF"
                "ENABLE_LIB_ONLY ON"
                "WITH_APP OFF"
                "ENABLE_ASIO_LIB OFF"
        )
    endif()

    # MsQuic (HTTP/3 - optional)
    if (FA_ENABLE_HTTP3)
        CPMAddPackage(
            NAME msquic
            GITHUB_REPOSITORY microsoft/msquic
            GIT_TAG v2.3.5
            OPTIONS "QUIC_TLS=openssl"
        )
    endif()
endif()

# -----------------------------
# PostgreSQL Dependencies
# -----------------------------

if (FA_BUILD_PG)
    # Find PostgreSQL development files (libpq)
    # We'll vendor libpq in phase 1, so this is optional
    find_package(PostgreSQL QUIET)
    if(PostgreSQL_FOUND)
        message(STATUS "Found PostgreSQL: ${PostgreSQL_VERSION_STRING}")
        include_directories(${PostgreSQL_INCLUDE_DIRS})
    else()
        message(STATUS "PostgreSQL not found, using vendored libpq stub")
        # In phase 1, we stub libpq integration
        include_directories(${CMAKE_SOURCE_DIR}/vendor/libpq/include)
    endif()
endif()

# -----------------------------
# PostgreSQL Library Target
# -----------------------------

if (FA_BUILD_PG)
    set(PG_SOURCES
        src/cpp/pg/pg_lib.cpp
        src/cpp/pg/pg_pool.cpp
        src/cpp/pg/pg_connection.cpp
        src/cpp/pg/pg_pool_impl.cpp
        src/cpp/pg/pg_connection_impl.cpp
        src/cpp/pg/pg_result.cpp
        src/cpp/pg/pg_future.cpp
    )

    set(PG_HEADERS
        src/cpp/pg/pg_pool.h
        src/cpp/pg/pg_connection.h
        src/cpp/pg/pg_protocol.h
        src/cpp/pg/pg_codec.h
        src/cpp/pg/pg_pool_impl.h
        src/cpp/pg/pg_connection_impl.h
        src/cpp/pg/pg_result.h
        src/cpp/pg/pg_future.h
    )

    # Create PostgreSQL shared library
    add_library(fasterapi_pg SHARED ${PG_SOURCES} ${PG_HEADERS} ${CORE_SOURCES} ${CORE_HEADERS})

    # Link PostgreSQL if available
    if(PostgreSQL_FOUND)
        target_link_libraries(fasterapi_pg PUBLIC ${PostgreSQL_LIBRARIES})
        target_include_directories(fasterapi_pg PRIVATE ${PostgreSQL_INCLUDE_DIRS})
    else()
        # Try to find PostgreSQL manually
        find_library(PQ_LIBRARY pq)
        if(PQ_LIBRARY)
            target_link_libraries(fasterapi_pg PUBLIC ${PQ_LIBRARY})
        else()
            # Fallback to explicit path
            target_link_libraries(fasterapi_pg PUBLIC /opt/homebrew/Cellar/postgresql@14/14.19/lib/postgresql@14/libpq.a)
            target_include_directories(fasterapi_pg PRIVATE /opt/homebrew/Cellar/postgresql@14/14.19/include/postgresql@14)
        endif()
    endif()

    # Set output directory
    set_target_properties(fasterapi_pg PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PREFIX "lib"
        SUFFIX ".${LIB_EXTENSION}"
    )

    # Strip symbols in release build
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        if(APPLE)
            add_custom_command(TARGET fasterapi_pg POST_BUILD
                COMMAND strip -x $<TARGET_FILE:fasterapi_pg>
                COMMENT "Stripping symbols from PostgreSQL library"
            )
        elseif(UNIX)
            add_custom_command(TARGET fasterapi_pg POST_BUILD
                COMMAND strip $<TARGET_FILE:fasterapi_pg>
                COMMENT "Stripping symbols from PostgreSQL library"
            )
        endif()
    endif()

    message(STATUS "PostgreSQL library target: fasterapi_pg")
    message(STATUS "Output: ${CMAKE_BINARY_DIR}/lib/libfasterapi_pg.${LIB_EXTENSION}")
endif()

# -----------------------------
# HTTP Server Library Target
# -----------------------------

if (FA_BUILD_HTTP)
    set(HTTP_SOURCES
        src/cpp/http/http_lib.cpp
        src/cpp/http/server.cpp
        src/cpp/http/request.cpp
        src/cpp/http/response.cpp
        src/cpp/http/router.cpp
        src/cpp/http/sse.cpp
    )
    
    # Add HTTP/2 handler if enabled
    if (FA_ENABLE_HTTP2)
        list(APPEND HTTP_SOURCES src/cpp/http/h2_handler.cpp)
    endif()
    
    # Add HTTP/3 handler if enabled
    if (FA_ENABLE_HTTP3)
        list(APPEND HTTP_SOURCES src/cpp/http/h3_handler.cpp)
    endif()
    
    # Add middleware
    list(APPEND HTTP_SOURCES src/cpp/http/middleware.cpp)
    
    # Add JSON parser
    list(APPEND HTTP_SOURCES src/cpp/http/json_parser.cpp)
    
    # Add HTTP parser
    list(APPEND HTTP_SOURCES src/cpp/http/http_parser.cpp)
    
    # Add health monitor
    list(APPEND HTTP_SOURCES src/cpp/http/health_monitor.cpp)
    
    # Add SSE support
    list(APPEND HTTP_SOURCES src/cpp/http/sse.cpp)
    
    # Add HPACK support (zero-allocation HTTP/2 header compression)
    list(APPEND HTTP_SOURCES src/cpp/http/hpack.cpp)
    
    # Add Huffman coding (for HPACK compression)
    list(APPEND HTTP_SOURCES src/cpp/http/huffman.cpp)
    
    # Add HTTP/2 Server Push
    list(APPEND HTTP_SOURCES src/cpp/http/h2_server_push.cpp)
    
    # Add HTTP/1.1 parser (zero-allocation)
    list(APPEND HTTP_SOURCES src/cpp/http/http1_parser.cpp)
    
    # Add HTTP/3 parser (zero-allocation)
    list(APPEND HTTP_SOURCES src/cpp/http/http3_parser.cpp)
    
    # Add WebRTC support (Pion-inspired + Aeron buffers)
    list(APPEND HTTP_SOURCES 
        src/cpp/webrtc/sdp_parser.cpp
        src/cpp/webrtc/ice.cpp
        src/cpp/webrtc/signaling.cpp
        src/cpp/webrtc/message_parser.cpp
        src/cpp/webrtc/data_channel.cpp
        src/cpp/webrtc/rtp.cpp
    )

    set(HTTP_HEADERS
        src/cpp/http/server.h
        src/cpp/http/router.h
        src/cpp/http/request.h
        src/cpp/http/response.h
        src/cpp/http/websocket.h
        src/cpp/http/middleware.h
        src/cpp/http/compression.h
    )
    
    # Add HTTP/2 handler if enabled
    if (FA_ENABLE_HTTP2)
        list(APPEND HTTP_HEADERS src/cpp/http/h2_handler.h)
    endif()
    
    # Add HTTP/3 handler if enabled
    if (FA_ENABLE_HTTP3)
        list(APPEND HTTP_HEADERS src/cpp/http/h3_handler.h)
    endif()
    
    # Add JSON parser
    list(APPEND HTTP_HEADERS src/cpp/http/json_parser.h)
    
    # Add HTTP parser
    list(APPEND HTTP_HEADERS src/cpp/http/http_parser.h)
    
    # Add health monitor
    list(APPEND HTTP_HEADERS src/cpp/http/health_monitor.h)
    
    # Add SSE support
    list(APPEND HTTP_HEADERS src/cpp/http/sse.h)
    
    # Add HPACK support
    list(APPEND HTTP_HEADERS src/cpp/http/hpack.h)
    
    # Add Huffman coding
    list(APPEND HTTP_HEADERS src/cpp/http/huffman.h)
    
    # Add HTTP/2 Server Push
    list(APPEND HTTP_HEADERS src/cpp/http/h2_server_push.h)
    
    # Add HTTP/1.1 parser
    list(APPEND HTTP_HEADERS src/cpp/http/http1_parser.h)
    
    # Add HTTP/3 parser
    list(APPEND HTTP_HEADERS src/cpp/http/http3_parser.h)
    
    # Add WebRTC support
    list(APPEND HTTP_HEADERS
        src/cpp/webrtc/sdp_parser.h
        src/cpp/webrtc/ice.h
        src/cpp/webrtc/signaling.h
        src/cpp/webrtc/message_parser.h
        src/cpp/webrtc/data_channel.h
        src/cpp/webrtc/rtp.h
    )

    # Create HTTP server shared library
    add_library(fasterapi_http SHARED ${HTTP_SOURCES} ${HTTP_HEADERS} ${CORE_SOURCES} ${CORE_HEADERS} ${PYTHON_SOURCES} ${PYTHON_HEADERS} ${NATIVE_TYPES_SOURCES} ${NATIVE_TYPES_HEADERS})

    # Add compile definitions
    target_compile_definitions(fasterapi_http PRIVATE
        FA_BUILD_HTTP=1
    )
    
    if (FA_USE_UWEBSOCKETS)
        target_compile_definitions(fasterapi_http PRIVATE FA_USE_UWEBSOCKETS=1)
        target_include_directories(fasterapi_http PRIVATE
            ${CMAKE_BINARY_DIR}/cpm-cache/uwebsockets/2d18d741850ab50ce1ba644223c67d92f4b33d01/src
        )
        
        # Link uWebSockets (and its deps)
        if (TARGET uWS)
            target_link_libraries(fasterapi_http PRIVATE uWS)
        elseif (TARGET uwebsockets)
            target_link_libraries(fasterapi_http PRIVATE uwebsockets)
        endif()
    endif()

    # Link libuv (Windows required, Linux/macOS optional)
    if (FA_USE_LIBUV)
        if (TARGET uv_a)
            target_link_libraries(fasterapi_http PRIVATE uv_a)
        elseif (TARGET uv)
            target_link_libraries(fasterapi_http PRIVATE uv)
        endif()
    endif()

    # Link simdjson
    target_link_libraries(fasterapi_http PRIVATE simdjson::simdjson)
    
    # Link Python
    target_include_directories(fasterapi_http PRIVATE ${Python3_INCLUDE_DIRS})
    target_link_libraries(fasterapi_http PRIVATE ${Python3_LIBRARIES})

    # Link zstd if compression enabled
    if (FA_ENABLE_COMPRESSION)
        if (TARGET libzstd_static)
            target_link_libraries(fasterapi_http PRIVATE libzstd_static)
        elseif (TARGET zstd)
            target_link_libraries(fasterapi_http PRIVATE zstd)
        endif()
        target_compile_definitions(fasterapi_http PRIVATE FA_COMPRESSION_ENABLED)
    endif()

    # Link nghttp2 if HTTP/2 enabled
    if (FA_ENABLE_HTTP2 AND TARGET nghttp2)
        target_link_libraries(fasterapi_http PRIVATE nghttp2)
        target_link_libraries(fasterapi_http PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(fasterapi_http PRIVATE FA_HTTP2_ENABLED)
    endif()

    # Link MsQuic if HTTP/3 enabled
    if (FA_ENABLE_HTTP3 AND TARGET msquic)
        target_link_libraries(fasterapi_http PRIVATE msquic)
        target_link_libraries(fasterapi_http PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(fasterapi_http PRIVATE FA_HTTP3_ENABLED)
    endif()

    # Windows socket libs
    if (WIN32)
        target_link_libraries(fasterapi_http PRIVATE ws2_32 userenv iphlpapi)
        target_compile_definitions(fasterapi_http PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    # Set output directory
    set_target_properties(fasterapi_http PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PREFIX "lib"
        SUFFIX ".${LIB_EXTENSION}"
    )

    # Strip symbols in release build
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        if(APPLE)
            add_custom_command(TARGET fasterapi_http POST_BUILD
                COMMAND strip -x $<TARGET_FILE:fasterapi_http>
                COMMENT "Stripping symbols from HTTP library"
            )
        elseif(UNIX)
            add_custom_command(TARGET fasterapi_http POST_BUILD
                COMMAND strip $<TARGET_FILE:fasterapi_http>
                COMMENT "Stripping symbols from HTTP library"
            )
        endif()
    endif()

    message(STATUS "HTTP server library target: fasterapi_http")
    message(STATUS "Output: ${CMAKE_BINARY_DIR}/lib/libfasterapi_http.${LIB_EXTENSION}")
endif()

# -----------------------------
# Shared Configuration
# -----------------------------

# Link mimalloc to both libraries if enabled
if (FA_ENABLE_MIMALLOC)
    if (WIN32)
        # Dynamic override on Windows
        if (FA_BUILD_PG)
            target_link_libraries(fasterapi_pg PRIVATE mimalloc)
            target_compile_definitions(fasterapi_pg PRIVATE MI_MALLOC_OVERRIDE)
        endif()
        if (FA_BUILD_HTTP)
            target_link_libraries(fasterapi_http PRIVATE mimalloc)
            target_compile_definitions(fasterapi_http PRIVATE MI_MALLOC_OVERRIDE)
        endif()
    else()
        # Static link on Linux/macOS
        if (FA_BUILD_PG)
            if (TARGET mimalloc-static)
                target_link_libraries(fasterapi_pg PRIVATE mimalloc-static)
            else()
                target_link_libraries(fasterapi_pg PRIVATE mimalloc)
            endif()
            target_compile_definitions(fasterapi_pg PRIVATE MI_MALLOC_OVERRIDE)
        endif()
        if (FA_BUILD_HTTP)
            if (TARGET mimalloc-static)
                target_link_libraries(fasterapi_http PRIVATE mimalloc-static)
            else()
                target_link_libraries(fasterapi_http PRIVATE mimalloc)
            endif()
            target_compile_definitions(fasterapi_http PRIVATE MI_MALLOC_OVERRIDE)
        endif()
    endif()
endif()

# Sanitizers (development only)
if (FA_SANITIZE AND NOT MSVC)
    if (FA_BUILD_PG)
        target_compile_options(fasterapi_pg PRIVATE -fsanitize=address,undefined)
        target_link_options(fasterapi_pg PRIVATE -fsanitize=address,undefined)
    endif()
    if (FA_BUILD_HTTP)
        target_compile_options(fasterapi_http PRIVATE -fsanitize=address,undefined)
        target_link_options(fasterapi_http PRIVATE -fsanitize=address,undefined)
    endif()
endif()

# -----------------------------
# Router Test Executable
# -----------------------------

if (FA_BUILD_HTTP)
    add_executable(test_router tests/test_router.cpp src/cpp/http/router.cpp)
    target_include_directories(test_router PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_router PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    add_executable(bench_router benchmarks/fasterapi/bench_router.cpp src/cpp/http/router.cpp)
    target_include_directories(bench_router PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(bench_router PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    
    add_executable(test_executor tests/test_executor.cpp ${PYTHON_SOURCES} ${CORE_SOURCES})
    target_include_directories(test_executor PRIVATE ${CMAKE_SOURCE_DIR} ${Python3_INCLUDE_DIRS})
    target_link_libraries(test_executor PRIVATE ${Python3_LIBRARIES})
    set_target_properties(test_executor PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    add_executable(test_hpack tests/test_hpack.cpp src/cpp/http/hpack.cpp)
    target_include_directories(test_hpack PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_hpack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    add_executable(bench_hpack benchmarks/fasterapi/bench_hpack.cpp src/cpp/http/hpack.cpp)
    target_include_directories(bench_hpack PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(bench_hpack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    
    add_executable(test_http1_parser tests/test_http1_parser.cpp src/cpp/http/http1_parser.cpp)
    target_include_directories(test_http1_parser PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_http1_parser PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    add_executable(bench_http1_parser benchmarks/fasterapi/bench_http1_parser.cpp src/cpp/http/http1_parser.cpp)
    target_include_directories(bench_http1_parser PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(bench_http1_parser PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    
    add_executable(test_http3_parser tests/test_http3_parser.cpp src/cpp/http/http3_parser.cpp)
    target_include_directories(test_http3_parser PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_http3_parser PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    add_executable(test_webrtc tests/test_webrtc.cpp 
        src/cpp/webrtc/sdp_parser.cpp
        src/cpp/webrtc/ice.cpp
        src/cpp/webrtc/signaling.cpp
        src/cpp/webrtc/message_parser.cpp
    )
    target_include_directories(test_webrtc PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_webrtc PRIVATE simdjson::simdjson)
    set_target_properties(test_webrtc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    add_executable(test_webrtc_media tests/test_webrtc_media.cpp 
        src/cpp/webrtc/rtp.cpp
        src/cpp/webrtc/data_channel.cpp
        src/cpp/core/ring_buffer.cpp
    )
    target_include_directories(test_webrtc_media PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_webrtc_media PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    add_executable(test_h2_server_push tests/test_h2_server_push.cpp 
        src/cpp/http/h2_server_push.cpp
        src/cpp/http/hpack.cpp
        src/cpp/http/huffman.cpp
    )
    target_include_directories(test_h2_server_push PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_h2_server_push PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    add_executable(test_native_types tests/test_native_types.cpp 
        ${NATIVE_TYPES_SOURCES}
        src/cpp/http/http1_parser.cpp
    )
    target_include_directories(test_native_types PRIVATE ${CMAKE_SOURCE_DIR} ${Python3_INCLUDE_DIRS})
    target_link_libraries(test_native_types PRIVATE ${Python3_LIBRARIES} simdjson::simdjson)
    set_target_properties(test_native_types PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    # Test lock-free queue
    add_executable(test_lockfree_queue tests/test_lockfree_queue.cpp)
    target_include_directories(test_lockfree_queue PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_lockfree_queue PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    # Test PyObject pool
    add_executable(test_pyobject_pool tests/test_pyobject_pool.cpp)
    target_include_directories(test_pyobject_pool PRIVATE ${CMAKE_SOURCE_DIR} ${Python3_INCLUDE_DIRS})
    target_link_libraries(test_pyobject_pool PRIVATE ${Python3_LIBRARIES})
    set_target_properties(test_pyobject_pool PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    # Test zero-copy response
    add_executable(test_zerocopy_response tests/test_zerocopy_response.cpp)
    target_include_directories(test_zerocopy_response PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_zerocopy_response PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    
    # Pure C++ benchmark for Python overhead analysis
    add_executable(bench_pure_cpp benchmarks/fasterapi/bench_pure_cpp.cpp
        src/cpp/http/router.cpp
        src/cpp/http/http1_parser.cpp
        src/cpp/http/hpack.cpp
        src/cpp/core/reactor.cpp
    )
    target_include_directories(bench_pure_cpp PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(bench_pure_cpp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    
    # TechEmpower benchmarks (pure C++)
    add_executable(bench_techempower_cpp benchmarks/techempower/techempower_cpp.cpp
        src/cpp/http/router.cpp
        src/cpp/http/http1_parser.cpp
        src/cpp/http/hpack.cpp
        ${NATIVE_TYPES_SOURCES}
    )
    target_include_directories(bench_techempower_cpp PRIVATE ${CMAKE_SOURCE_DIR} ${Python3_INCLUDE_DIRS})
    target_link_libraries(bench_techempower_cpp PRIVATE ${Python3_LIBRARIES} simdjson::simdjson)
    set_target_properties(bench_techempower_cpp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    
    # TechEmpower concurrent benchmarks (multithreaded)
    add_executable(bench_techempower_concurrent benchmarks/techempower/techempower_concurrent.cpp
        src/cpp/http/router.cpp
        src/cpp/http/http1_parser.cpp
        ${CORE_SOURCES}
    )
    target_include_directories(bench_techempower_concurrent PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(bench_techempower_concurrent PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    
    # 1 Million Request Challenge (1MRC) - Pure C++ server
    add_executable(1mrc_cpp_server benchmarks/1mrc/cpp/1mrc_cpp_server.cpp)
    target_include_directories(1mrc_cpp_server PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(1mrc_cpp_server PRIVATE -O3 -pthread)
    target_link_libraries(1mrc_cpp_server PRIVATE pthread)
    set_target_properties(1mrc_cpp_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    
    # 1MRC - Async I/O version (kqueue/epoll/io_uring/IOCP)
    add_executable(1mrc_async_server benchmarks/1mrc/cpp/1mrc_async_server.cpp
        ${CORE_SOURCES}
    )
    target_include_directories(1mrc_async_server PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(1mrc_async_server PRIVATE -O3 -pthread)
    target_link_libraries(1mrc_async_server PRIVATE pthread)
    set_target_properties(1mrc_async_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
    
    # 1MRC - libuv version (optimized, production-ready)
    add_executable(1mrc_libuv_server benchmarks/1mrc/cpp/1mrc_libuv_server.cpp)
    target_include_directories(1mrc_libuv_server PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(1mrc_libuv_server PRIVATE -O3 -pthread)
    target_link_libraries(1mrc_libuv_server PRIVATE uv_a pthread)
    set_target_properties(1mrc_libuv_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
endif()
