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
option(FA_BUILD_MCP          "Build MCP (Model Context Protocol) support" ON)
# option(FA_USE_LIBUV          "Use libuv for event loop" OFF)  # REMOVED: FasterAPI uses native kqueue/epoll event loops
option(FA_USE_ASIO           "Build with standalone Asio (for Beast/H2 path)" OFF)
option(FA_ENABLE_HTTP2       "Enable HTTP/2 support (pure C++ implementation)" ON)
option(FA_ENABLE_HTTP3       "Enable HTTP/3 support (pure C++ QUIC + QPACK + WebTransport)" ON)
option(FA_ENABLE_COMPRESSION "Enable zstd compression" ON)
option(FA_ENABLE_MIMALLOC    "Link mimalloc as default allocator" OFF)  # Disabled: causes memory corruption on macOS with shared libraries
option(FA_USE_UWEBSOCKETS    "Use uWebSockets for HTTP/1.1 and WebSocket" OFF)
option(FA_SANITIZE           "Enable Address/UB sanitizers (non-MSVC)" OFF)
option(ENABLE_EXCEPTIONS     "Enable C++ exceptions (default: OFF for performance)" OFF)
option(FA_BUILD_GTEST        "Build tests with Google Test framework" ON)
option(FA_BUILD_GBENCH       "Build benchmarks with Google Benchmark" ON)
option(FA_ENABLE_COVERAGE    "Enable code coverage (gcov/lcov)" OFF)
option(FA_NATIVE_ARCH        "Enable -march=native/-mcpu=native (disable for CI)" ON)

# Optimization flags for maximum performance
# Use -mcpu=native on Apple, -march=native on other platforms
# Disable with FA_NATIVE_ARCH=OFF for CI builds
if(FA_NATIVE_ARCH)
    if(APPLE)
        set(NATIVE_FLAG "-mcpu=native")
    else()
        set(NATIVE_FLAG "-march=native")
    endif()
else()
    set(NATIVE_FLAG "")
    message(STATUS "Native architecture optimizations disabled (FA_NATIVE_ARCH=OFF)")
endif()

# Coverage flags
if(FA_ENABLE_COVERAGE AND NOT MSVC)
    set(COVERAGE_FLAGS "--coverage -fprofile-arcs -ftest-coverage")
    message(STATUS "Code coverage enabled")
else()
    set(COVERAGE_FLAGS "")
endif()

# Conditional exception flags
if(NOT ENABLE_EXCEPTIONS)
    set(EXCEPTION_FLAGS "-fno-exceptions -fno-rtti")
else()
    set(EXCEPTION_FLAGS "")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 ${NATIVE_FLAG} -flto ${EXCEPTION_FLAGS} ${COVERAGE_FLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g ${EXCEPTION_FLAGS} ${COVERAGE_FLAGS}")

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
    src/cpp/core/logger.cpp
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
    src/cpp/python/free_threading.cpp
    src/cpp/python/shared_memory_ipc.cpp
    src/cpp/python/process_pool_executor.cpp
    src/cpp/python/zmq_ipc.cpp
)

set(PYTHON_HEADERS
    src/cpp/python/gil_guard.h
    src/cpp/python/py_executor.h
    src/cpp/python/free_threading.h
    src/cpp/python/pyobject_pool.h
    src/cpp/python/shared_memory_ipc.h
    src/cpp/python/process_pool_executor.h
    src/cpp/python/zmq_ipc.h
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

# Check Python version for advanced features
if (Python3_VERSION_MAJOR EQUAL 3 AND Python3_VERSION_MINOR GREATER_EQUAL 13)
    message(STATUS "Python 3.13+ detected: Free-threading support available")
    message(STATUS "  - Check if built with --disable-gil for best performance")
    message(STATUS "  - Expected ~60x speedup on ${CMAKE_SYSTEM_PROCESSOR} for CPU-bound workloads")
    add_compile_definitions(FASTERAPI_FREE_THREADING_AVAILABLE)
endif()

# OpenSSL (required for TLS, ALPN)
if (FA_ENABLE_HTTP2 OR FA_ENABLE_HTTP3)
    find_package(OpenSSL REQUIRED)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
endif()

# ZeroMQ (optional IPC transport for multi-language workers)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ZMQ libzmq)
    if(ZMQ_FOUND)
        message(STATUS "ZeroMQ found: ${ZMQ_VERSION}")
        message(STATUS "  - Optional IPC transport available for multi-language workers")
        message(STATUS "  - Shared memory IPC remains default for best performance")
    else()
        message(STATUS "ZeroMQ not found - using shared memory IPC only")
    endif()
else()
    message(STATUS "pkg-config not found - ZeroMQ detection skipped")
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
    # NOTE: libuv REMOVED - FasterAPI uses native kqueue (macOS) / epoll (Linux) event loops
    # See: src/cpp/net/event_loop_kqueue.cpp, event_loop_epoll.cpp

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
        OPTIONS
            "SIMDJSON_BUILD_STATIC ON"
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

    # HTTP/3 Support:
    # - Pure C++ QUIC implementation (RFC 9000)
    # - QPACK header compression (RFC 9204)
    # - HTTP/3 framing (RFC 9114)
    # - WebTransport (RFC 9297)
    # - No external dependencies (no MsQuic, no quiche)
    # - Zero-allocation design with ring buffers and object pools
endif()

# -----------------------------
# Testing Framework (Google Test)
# -----------------------------

if(FA_BUILD_GTEST)
    CPMAddPackage(
        NAME googletest
        GITHUB_REPOSITORY google/googletest
        GIT_TAG v1.14.0
        OPTIONS
            "BUILD_GMOCK ON"
            "INSTALL_GTEST OFF"
            "gtest_force_shared_crt ON"
    )
    include(GoogleTest)
    enable_testing()
    message(STATUS "Google Test enabled")
endif()

# -----------------------------
# Benchmarking Framework (Google Benchmark)
# -----------------------------

if(FA_BUILD_GBENCH)
    CPMAddPackage(
        NAME benchmark
        GITHUB_REPOSITORY google/benchmark
        GIT_TAG v1.8.3
        OPTIONS
            "BENCHMARK_ENABLE_TESTING OFF"
            "BENCHMARK_ENABLE_GTEST_TESTS OFF"
            "BENCHMARK_ENABLE_INSTALL OFF"
    )
    message(STATUS "Google Benchmark enabled")
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

# Net layer sources (event loop, TCP, UDP, TLS)
set(NET_SOURCES
    src/cpp/net/event_loop.cpp
    src/cpp/net/tcp_socket.cpp
    src/cpp/net/tcp_listener.cpp
    src/cpp/net/udp_socket.cpp
    src/cpp/net/udp_listener.cpp
    src/cpp/net/tls_context.cpp
    src/cpp/net/tls_cert_generator.cpp
    src/cpp/net/tls_socket.cpp
)

# Platform-specific event loop implementations
if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    list(APPEND NET_SOURCES src/cpp/net/event_loop_kqueue.cpp)
    message(STATUS "Event loop backend: kqueue (macOS/BSD)")
elseif(UNIX AND NOT WIN32)
    # Linux - epoll and optionally io_uring
    list(APPEND NET_SOURCES src/cpp/net/event_loop_epoll.cpp)
    message(STATUS "Event loop backend: epoll (Linux)")
elseif(WIN32)
    # Windows - IOCP (I/O Completion Ports)
    list(APPEND NET_SOURCES src/cpp/net/event_loop_iocp.cpp)
    message(STATUS "Event loop backend: IOCP (Windows)")
endif()

if (FA_BUILD_HTTP)
    set(HTTP_SOURCES
        # Note: http_lib.cpp and server.cpp removed - old CoroIO backend
        # Replaced by http2_server.cpp with native event loop
        src/cpp/http/request.cpp
        src/cpp/http/response.cpp
        src/cpp/http/router.cpp
        src/cpp/http/parameter_extractor.cpp
        src/cpp/http/schema_validator.cpp
        src/cpp/http/route_metadata.cpp
        src/cpp/http/validation_error_formatter.cpp
        src/cpp/http/openapi_generator.cpp
        src/cpp/http/static_docs.cpp
        src/cpp/http/sse.cpp
    )

    # Add HTTP/2 handler if enabled
    if (FA_ENABLE_HTTP2)
        list(APPEND HTTP_SOURCES src/cpp/http/h2_handler.cpp)
    endif()

    # Add HTTP/3 handler if enabled
    if (FA_ENABLE_HTTP3)
        list(APPEND HTTP_SOURCES
            src/cpp/http/h3_handler.cpp
            src/cpp/http/http3_connection.cpp
            # QUIC implementation (pure, no external deps)
            src/cpp/http/quic/quic_packet.cpp
            src/cpp/http/quic/quic_stream.cpp
            src/cpp/http/quic/quic_connection.cpp
            src/cpp/http/quic/quic_flow_control.cpp
            src/cpp/http/quic/quic_congestion.cpp
            src/cpp/http/quic/quic_ack_tracker.cpp
            # QPACK implementation
            src/cpp/http/qpack/qpack_encoder.cpp
            src/cpp/http/qpack/qpack_decoder.cpp
            src/cpp/http/qpack/qpack_static_table.cpp
            src/cpp/http/qpack/qpack_dynamic_table.cpp
            # WebTransport implementation (RFC 9297)
            src/cpp/http/webtransport_connection.cpp
        )
        message(STATUS "HTTP/3: Pure QUIC + QPACK implementation (no MsQuic)")
        message(STATUS "WebTransport: RFC 9297 implementation (streams + datagrams)")
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
    list(APPEND HTTP_SOURCES src/cpp/http/sse_lib.cpp)

    # Add WebSocket support (from scratch, inspired by uWebSockets)
    list(APPEND HTTP_SOURCES src/cpp/http/websocket_parser.cpp)
    list(APPEND HTTP_SOURCES src/cpp/http/websocket.cpp)
    list(APPEND HTTP_SOURCES src/cpp/http/websocket_lib.cpp)

    # Add HPACK support (zero-allocation HTTP/2 header compression)
    list(APPEND HTTP_SOURCES src/cpp/http/hpack.cpp)

    # Add Huffman coding (for HPACK compression)
    list(APPEND HTTP_SOURCES src/cpp/http/huffman.cpp)
    list(APPEND HTTP_SOURCES src/cpp/http/huffman_table_data.cpp)

    # Add HTTP/2 Server Push
    list(APPEND HTTP_SOURCES src/cpp/http/h2_server_push.cpp)

    # Add HTTP Server C API (for Python ctypes bindings)
    list(APPEND HTTP_SOURCES src/cpp/http/http_server_c_api.cpp)

    # Add HTTP/1.1 parser (zero-allocation)
    list(APPEND HTTP_SOURCES src/cpp/http/http1_parser.cpp)

    # Add HTTP/1.1 connection handler
    list(APPEND HTTP_SOURCES src/cpp/http/http1_connection.cpp)
    # Note: http1_coroio_handler.cpp removed - CoroIO dependency removed

    # Add unified HTTP server (TLS/ALPN multi-protocol)
    list(APPEND HTTP_SOURCES src/cpp/http/unified_server.cpp)

    # Note: Removed CoroIO-based handlers - using native event loop instead

    # Add Python callback bridge
    list(APPEND HTTP_SOURCES src/cpp/http/python_callback_bridge.cpp)

    # Add HTTP/2 native server (with Python integration)
    if (FA_ENABLE_HTTP2)
        list(APPEND HTTP_SOURCES src/cpp/http/http2_server.cpp)
        list(APPEND HTTP_SOURCES src/cpp/http/http2_frame.cpp)
        list(APPEND HTTP_SOURCES src/cpp/http/http2_stream.cpp)
        list(APPEND HTTP_SOURCES src/cpp/http/http2_connection.cpp)
    endif()

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

    # Add high-level C++ user API (exception-free since 2025-01-25)
    list(APPEND HTTP_SOURCES src/cpp/http/app.cpp)
    list(APPEND HTTP_SOURCES src/cpp/http/server.cpp)

    set(HTTP_HEADERS
        src/cpp/http/server.h
        src/cpp/http/router.h
        src/cpp/http/request.h
        src/cpp/http/response.h
        src/cpp/http/websocket.h
        src/cpp/http/middleware.h
        src/cpp/http/compression.h
        src/cpp/http/app.h
    )

    # Add HTTP/2 handler if enabled
    if (FA_ENABLE_HTTP2)
        list(APPEND HTTP_HEADERS src/cpp/http/h2_handler.h)
    endif()

    # Add HTTP/3 handler if enabled
    if (FA_ENABLE_HTTP3)
        list(APPEND HTTP_HEADERS src/cpp/http/h3_handler.h)
        list(APPEND HTTP_HEADERS src/cpp/http/http3_connection.h)
        list(APPEND HTTP_HEADERS src/cpp/http/webtransport_connection.h)
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

    # Note: CoroIO removed - using native event loop (kqueue/epoll) instead
    # Native event loop achieves 160K+ req/s vs CoroIO's 600 req/s

    # Create HTTP server shared library
    add_library(fasterapi_http SHARED ${NET_SOURCES} ${HTTP_SOURCES} ${HTTP_HEADERS} ${CORE_SOURCES} ${CORE_HEADERS} ${PYTHON_SOURCES} ${PYTHON_HEADERS} ${NATIVE_TYPES_SOURCES} ${NATIVE_TYPES_HEADERS})

    # Add source directory to include path
    target_include_directories(fasterapi_http PRIVATE ${CMAKE_SOURCE_DIR}/src/cpp)

    # Add compile definitions
    target_compile_definitions(fasterapi_http PRIVATE
        FA_BUILD_HTTP=1
        FASTERAPI_ENABLE_LOGGING
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

    # REMOVED: libuv not used - FasterAPI uses native kqueue/epoll event loops
    # To restore, uncomment below and re-enable FA_USE_LIBUV option + CPMAddPackage
    # if (FA_USE_LIBUV)
    #     if (TARGET uv_a)
    #         target_link_libraries(fasterapi_http PRIVATE uv_a)
    #     elseif (TARGET uv)
    #         target_link_libraries(fasterapi_http PRIVATE uv)
    #     endif()
    # endif()

    # Link simdjson
    target_link_libraries(fasterapi_http PRIVATE simdjson)

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

    # Set HTTP/2 flag if enabled (pure C++ implementation, no external deps)
    if (FA_ENABLE_HTTP2)
        target_compile_definitions(fasterapi_http PRIVATE FA_HTTP2_ENABLED)
    endif()

    # Link OpenSSL (required for WebSockets, h2_handler, HTTP/3)
    if (OPENSSL_FOUND)
        target_link_libraries(fasterapi_http PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()

    # Link ZeroMQ if available (optional IPC transport)
    if(ZMQ_FOUND)
        target_compile_definitions(fasterapi_http PRIVATE FASTERAPI_USE_ZMQ)
        target_include_directories(fasterapi_http PRIVATE ${ZMQ_INCLUDE_DIRS})
        target_link_directories(fasterapi_http PRIVATE ${ZMQ_LIBRARY_DIRS})
        target_link_libraries(fasterapi_http PRIVATE ${ZMQ_LIBRARIES})
    endif()

    # HTTP/3 enabled with pure implementation (no MsQuic)
    if (FA_ENABLE_HTTP3)
        target_compile_definitions(fasterapi_http PRIVATE FA_HTTP3_ENABLED)
        message(STATUS "HTTP/3 enabled with pure QUIC + QPACK implementation")
    endif()

    # Windows socket libs
    if (WIN32)
        # ws2_32 - Winsock2 core
        # mswsock - Microsoft Winsock extensions (AcceptEx, ConnectEx, etc. for IOCP)
        # userenv - User environment
        # iphlpapi - IP helper (network interfaces)
        target_link_libraries(fasterapi_http PRIVATE ws2_32 mswsock userenv iphlpapi)
        target_compile_definitions(fasterapi_http PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    # Set output directory
    set_target_properties(fasterapi_http PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PREFIX "lib"
        SUFFIX ".${LIB_EXTENSION}"
    )

    # Strip symbols in release build
    # TEMP: Disabled for debugging
    #if(CMAKE_BUILD_TYPE STREQUAL "Release")
    #    if(APPLE)
    #        add_custom_command(TARGET fasterapi_http POST_BUILD
    #            COMMAND strip -x $<TARGET_FILE:fasterapi_http>
    #            COMMENT "Stripping symbols from HTTP library"
    #        )
    #    elseif(UNIX)
    #        add_custom_command(TARGET fasterapi_http POST_BUILD
    #            COMMAND strip $<TARGET_FILE:fasterapi_http>
    #            COMMENT "Stripping symbols from HTTP library"
    #        )
    #    endif()
    #endif()

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
# Native Event Loop Test
# -----------------------------

# Test the native event loop with a simple echo server
add_executable(test_event_loop_echo tests/test_event_loop_echo.cpp ${NET_SOURCES})
target_include_directories(test_event_loop_echo PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_event_loop_echo PRIVATE OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(test_event_loop_echo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

# Test multi-threaded TCP listener
add_executable(test_tcp_listener_echo tests/test_tcp_listener_echo.cpp ${NET_SOURCES})
target_include_directories(test_tcp_listener_echo PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_tcp_listener_echo PRIVATE OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(test_tcp_listener_echo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)

# Test native HTTP/1.1 server
if (FA_BUILD_HTTP)
    add_executable(test_http1_native
        tests/test_http1_native.cpp
        ${NET_SOURCES}
        src/cpp/http/http1_parser.cpp
    )
    target_include_directories(test_http1_native PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_http1_native PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(test_http1_native PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    # Test native HTTP/2 server (disabled - needs nghttp2 library)
    # if (FA_ENABLE_HTTP2)
    #     add_executable(test_http2_native
    #         tests/test_http2_native.cpp
    #         ${NET_SOURCES}
    #     )
    #     target_include_directories(test_http2_native PRIVATE
    #         ${CMAKE_SOURCE_DIR}
    #         ${Python3_INCLUDE_DIRS}
    #     )
    #     target_link_libraries(test_http2_native PRIVATE OpenSSL::SSL OpenSSL::Crypto ${Python3_LIBRARIES})
    #     set_target_properties(test_http2_native PROPERTIES
    #         RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    #     )
    # endif()

    # Test unified server (TLS/ALPN multi-protocol)
    add_executable(test_unified_server
        tests/test_unified_server.cpp
    )
    target_include_directories(test_unified_server PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_unified_server PRIVATE fasterapi_http OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(test_unified_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
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

    add_executable(test_parameter_extractor tests/test_parameter_extractor.cpp src/cpp/http/parameter_extractor.cpp src/cpp/core/logger.cpp)
    target_include_directories(test_parameter_extractor PRIVATE ${CMAKE_SOURCE_DIR}/src/cpp)
    set_target_properties(test_parameter_extractor PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    add_executable(bench_router benchmarks/fasterapi/bench_router.cpp src/cpp/http/router.cpp)
    target_include_directories(bench_router PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(bench_router PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )

    add_executable(test_hpack tests/test_hpack.cpp src/cpp/http/hpack.cpp)
    target_include_directories(test_hpack PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_hpack PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    add_executable(test_qpack_static_table tests/test_qpack_static_table.cpp)
    target_include_directories(test_qpack_static_table PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_qpack_static_table PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    add_executable(test_qpack_dynamic_table tests/test_qpack_dynamic_table.cpp)
    target_include_directories(test_qpack_dynamic_table PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_qpack_dynamic_table PROPERTIES
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

    add_executable(test_http3_performance tests/test_http3_performance.cpp
        src/cpp/http/http3_parser.cpp
        src/cpp/http/qpack/qpack_encoder.cpp
        src/cpp/http/qpack/qpack_decoder.cpp
        src/cpp/http/qpack/qpack_static_table.cpp
        src/cpp/http/qpack/qpack_dynamic_table.cpp
        src/cpp/http/huffman.cpp
        src/cpp/http/huffman_table.cpp
        src/cpp/http/huffman_table_data.cpp
    )
    target_include_directories(test_http3_performance PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_http3_performance PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    # HTTP/3 End-to-End Integration Tests (Full Stack)
    # Note: Uses QPACK without Huffman encoding (Huffman disabled via flag)
    add_executable(test_http3_integration tests/test_http3_integration.cpp
        # HTTP/3 Components
        src/cpp/http/h3_handler.cpp
        src/cpp/http/http3_parser.cpp
        # QUIC Components
        src/cpp/http/quic/quic_connection.cpp
        src/cpp/http/quic/quic_packet.cpp
        src/cpp/http/quic/quic_stream.cpp
        src/cpp/http/quic/quic_flow_control.cpp
        src/cpp/http/quic/quic_congestion.cpp
        src/cpp/http/quic/quic_ack_tracker.cpp
        # QPACK Components
        src/cpp/http/qpack/qpack_encoder.cpp
        src/cpp/http/qpack/qpack_decoder.cpp
        src/cpp/http/qpack/qpack_static_table.cpp
        src/cpp/http/qpack/qpack_dynamic_table.cpp
        # Core Components
        src/cpp/core/ring_buffer.cpp
        src/cpp/core/logger.cpp
        src/cpp/http/huffman.cpp
        src/cpp/http/huffman_table_data.cpp
    )
    target_include_directories(test_http3_integration PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_http3_integration PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    add_executable(test_http3_stress tests/test_http3_stress.cpp
        # HTTP/3 Components
        src/cpp/http/http3_parser.cpp
        # QUIC Components
        src/cpp/http/quic/quic_connection.cpp
        src/cpp/http/quic/quic_packet.cpp
        src/cpp/http/quic/quic_stream.cpp
        src/cpp/http/quic/quic_flow_control.cpp
        src/cpp/http/quic/quic_congestion.cpp
        src/cpp/http/quic/quic_ack_tracker.cpp
        # QPACK Components
        src/cpp/http/qpack/qpack_encoder.cpp
        src/cpp/http/qpack/qpack_decoder.cpp
        src/cpp/http/qpack/qpack_static_table.cpp
        src/cpp/http/qpack/qpack_dynamic_table.cpp
        # Core Components
        src/cpp/core/ring_buffer.cpp
        src/cpp/core/logger.cpp
        src/cpp/http/huffman.cpp
    )
    target_include_directories(test_http3_stress PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(test_http3_stress PRIVATE FASTERAPI_NO_HUFFMAN=1)
    set_target_properties(test_http3_stress PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    # HTTP/3 Interoperability Tests (multi-implementation compatibility)
    add_executable(test_http3_interop tests/test_http3_interop.cpp
        # HTTP/3 Components
        src/cpp/http/h3_handler.cpp
        src/cpp/http/http3_parser.cpp
        src/cpp/http/http3_connection.cpp
        # QUIC Components
        src/cpp/http/quic/quic_connection.cpp
        src/cpp/http/quic/quic_packet.cpp
        src/cpp/http/quic/quic_stream.cpp
        src/cpp/http/quic/quic_flow_control.cpp
        src/cpp/http/quic/quic_congestion.cpp
        src/cpp/http/quic/quic_ack_tracker.cpp
        # QPACK Components
        src/cpp/http/qpack/qpack_encoder.cpp
        src/cpp/http/qpack/qpack_decoder.cpp
        src/cpp/http/qpack/qpack_static_table.cpp
        src/cpp/http/qpack/qpack_dynamic_table.cpp
        # WebTransport
        src/cpp/http/webtransport_connection.cpp
        # Core Components
        src/cpp/core/ring_buffer.cpp
        src/cpp/core/logger.cpp
        src/cpp/http/huffman.cpp
        src/cpp/http/huffman_table_data.cpp
    )
    target_include_directories(test_http3_interop PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(test_http3_interop PRIVATE FA_HTTP3_ENABLED)
    set_target_properties(test_http3_interop PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    # HTTP/3 UnifiedServer Integration Tests (end-to-end testing)
    add_executable(test_http3_unified_integration tests/test_http3_unified_integration.cpp
        # HTTP/3 Components
        src/cpp/http/h3_handler.cpp
        src/cpp/http/http3_parser.cpp
        src/cpp/http/http3_connection.cpp
        # QUIC Components
        src/cpp/http/quic/quic_connection.cpp
        src/cpp/http/quic/quic_packet.cpp
        src/cpp/http/quic/quic_stream.cpp
        src/cpp/http/quic/quic_flow_control.cpp
        src/cpp/http/quic/quic_congestion.cpp
        src/cpp/http/quic/quic_ack_tracker.cpp
        # QPACK Components
        src/cpp/http/qpack/qpack_encoder.cpp
        src/cpp/http/qpack/qpack_decoder.cpp
        src/cpp/http/qpack/qpack_static_table.cpp
        src/cpp/http/qpack/qpack_dynamic_table.cpp
        # WebTransport
        src/cpp/http/webtransport_connection.cpp
        # Core Components
        src/cpp/core/ring_buffer.cpp
        src/cpp/core/logger.cpp
        src/cpp/http/huffman.cpp
    )
    target_include_directories(test_http3_unified_integration PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(test_http3_unified_integration PRIVATE FASTERAPI_NO_HUFFMAN=1 FA_HTTP3_ENABLED)
    target_link_libraries(test_http3_unified_integration PRIVATE fasterapi_http OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(test_http3_unified_integration PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    # HTTP/3 End-to-End Tests with App API
    # Tests the high-level FasterAPI App class with HTTP/3 enabled
    add_executable(test_app_http3_e2e tests/test_app_http3_e2e.cpp)
    target_include_directories(test_app_http3_e2e PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(test_app_http3_e2e PRIVATE FASTERAPI_NO_HUFFMAN=1 FA_HTTP3_ENABLED)
    target_link_libraries(test_app_http3_e2e PRIVATE fasterapi_http OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(test_app_http3_e2e PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    # App Destructor Unit Tests
    # Tests the App destructor for memory safety and proper cleanup
    add_executable(test_app_destructor tests/test_app_destructor.cpp)
    target_include_directories(test_app_destructor PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(test_app_destructor PRIVATE FASTERAPI_NO_HUFFMAN=1 FA_HTTP3_ENABLED)
    target_link_libraries(test_app_destructor PRIVATE fasterapi_http OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(test_app_destructor PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    # Multi-Protocol HTTP Verb Tests
    # Comprehensive testing of all HTTP methods across HTTP/1.1, HTTP/2, and HTTP/3
    # Verifies transparent protocol selection and clean user-facing API
    add_executable(test_multi_protocol_verbs tests/test_multi_protocol_verbs.cpp)
    target_include_directories(test_multi_protocol_verbs PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(test_multi_protocol_verbs PRIVATE FA_HTTP3_ENABLED)
    target_link_libraries(test_multi_protocol_verbs PRIVATE fasterapi_http OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(test_multi_protocol_verbs PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    # Debug test for App destructor crash
    add_executable(test_app_crash_debug tests/test_app_crash_debug.cpp)
    target_include_directories(test_app_crash_debug PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(test_app_crash_debug PRIVATE FASTERAPI_NO_HUFFMAN=1 FA_HTTP3_ENABLED)
    target_link_libraries(test_app_crash_debug PRIVATE fasterapi_http OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(test_app_crash_debug PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    add_executable(test_webrtc tests/test_webrtc.cpp
        src/cpp/webrtc/sdp_parser.cpp
        src/cpp/webrtc/ice.cpp
        src/cpp/webrtc/signaling.cpp
        src/cpp/webrtc/message_parser.cpp
    )
    target_include_directories(test_webrtc PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_webrtc PRIVATE simdjson)
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
        src/cpp/http/huffman_table_data.cpp
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
    target_link_libraries(test_native_types PRIVATE ${Python3_LIBRARIES} simdjson)
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

    # Test C++20 coroutine infrastructure
    add_executable(test_coro_task tests/test_coro_task.cpp)
    target_include_directories(test_coro_task PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(test_coro_task PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
    )

    # Note: test_cpp_routes removed - used old CoroIO backend
    # Use native HTTP/2 server with Cython bindings instead

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
    target_link_libraries(bench_techempower_cpp PRIVATE ${Python3_LIBRARIES} simdjson)
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

    # REMOVED: libuv benchmark - FasterAPI uses native kqueue/epoll event loops
    # To restore, uncomment below and re-enable FA_USE_LIBUV + CPMAddPackage(libuv)
    # add_executable(1mrc_libuv_server benchmarks/1mrc/cpp/1mrc_libuv_server.cpp)
    # target_include_directories(1mrc_libuv_server PRIVATE ${CMAKE_SOURCE_DIR})
    # target_compile_options(1mrc_libuv_server PRIVATE -O3 -pthread)
    # target_link_libraries(1mrc_libuv_server PRIVATE uv pthread)
    # set_target_properties(1mrc_libuv_server PROPERTIES
    #     RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    # )

    # Note: 1mrc_coroio_server removed - used old CoroIO backend
    # Native event loop (test_http1_native, test_http2_native) is 250x faster

    # 1MRC - Native lockfree version (production architecture)
    add_executable(1mrc_native_lockfree
        benchmarks/1mrc/cpp/1mrc_native_lockfree.cpp
        ${NET_SOURCES}
        src/cpp/http/http1_parser.cpp
    )
    target_include_directories(1mrc_native_lockfree PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_options(1mrc_native_lockfree PRIVATE -O3 -pthread)
    target_link_libraries(1mrc_native_lockfree PRIVATE OpenSSL::SSL OpenSSL::Crypto pthread)
    set_target_properties(1mrc_native_lockfree PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )

    # Python interop benchmarks
    add_executable(bench_gil_strategies benchmarks/python_interop/bench_gil_strategies.cpp
        ${PYTHON_SOURCES}
        ${CORE_SOURCES}
    )
    target_include_directories(bench_gil_strategies PRIVATE ${CMAKE_SOURCE_DIR} ${Python3_INCLUDE_DIRS})
    target_link_libraries(bench_gil_strategies PRIVATE ${Python3_LIBRARIES} pthread)
    set_target_properties(bench_gil_strategies PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )

    add_executable(bench_queue_performance benchmarks/python_interop/bench_queue_performance.cpp)
    target_include_directories(bench_queue_performance PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(bench_queue_performance PRIVATE pthread)
    set_target_properties(bench_queue_performance PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
endif()

# -----------------------------
# MCP (Model Context Protocol) Library
# -----------------------------

if (FA_BUILD_MCP)
    message(STATUS "Building MCP library")

    # MCP sources
    set(MCP_SOURCES
        src/cpp/mcp/protocol/message.cpp
        src/cpp/mcp/protocol/session.cpp
        src/cpp/mcp/transports/stdio_transport.cpp
        src/cpp/mcp/transports/sse_transport.cpp
        src/cpp/mcp/transports/websocket_transport.cpp
        src/cpp/mcp/client/mcp_client.cpp
        src/cpp/mcp/server/mcp_server.cpp
        src/cpp/mcp/security/auth.cpp
        src/cpp/mcp/security/rate_limit.cpp
        src/cpp/mcp/security/sandbox.cpp
        src/cpp/mcp/proxy/proxy_core.cpp
        src/cpp/mcp/proxy/upstream_connection.cpp
        src/cpp/mcp/mcp_lib.cpp
    )

    set(MCP_HEADERS
        src/cpp/mcp/protocol/message.h
        src/cpp/mcp/protocol/session.h
        src/cpp/mcp/transports/transport.h
        src/cpp/mcp/transports/stdio_transport.h
        src/cpp/mcp/server/mcp_server.h
        src/cpp/mcp/client/mcp_client.h
        src/cpp/mcp/security/auth.h
        src/cpp/mcp/security/rate_limit.h
        src/cpp/mcp/security/sandbox.h
        src/cpp/mcp/proxy/proxy_core.h
        src/cpp/mcp/proxy/upstream_connection.h
    )

    # Build shared library
    add_library(fasterapi_mcp SHARED ${MCP_SOURCES})

    target_include_directories(fasterapi_mcp
        PUBLIC ${CMAKE_SOURCE_DIR}/src/cpp
        PRIVATE ${CMAKE_SOURCE_DIR}
    )

    # Link OpenSSL for JWT/OAuth (required for security features)
    if (OPENSSL_FOUND)
        target_link_libraries(fasterapi_mcp PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    else()
        message(WARNING "OpenSSL not found - JWT authentication will be disabled")
    endif()

    set_target_properties(fasterapi_mcp PROPERTIES
        OUTPUT_NAME "fasterapi_mcp"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/fasterapi/_native"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/fasterapi/_native"
    )

    # Install library
    install(TARGETS fasterapi_mcp
        LIBRARY DESTINATION fasterapi/_native
        RUNTIME DESTINATION fasterapi/_native
    )

    message(STATUS "MCP library will be built at: ${CMAKE_SOURCE_DIR}/fasterapi/_native/libfasterapi_mcp.${LIB_EXTENSION}")

    # MCP unit tests
    add_executable(test_mcp_protocol tests/test_mcp_protocol.cpp)
    target_include_directories(test_mcp_protocol PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_mcp_protocol PRIVATE fasterapi_mcp)
    set_target_properties(test_mcp_protocol PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    add_executable(test_mcp_transport tests/test_mcp_transport.cpp)
    target_include_directories(test_mcp_transport PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_mcp_transport PRIVATE fasterapi_mcp)
    set_target_properties(test_mcp_transport PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    message(STATUS "MCP tests will be built at: ${CMAKE_BINARY_DIR}/tests/test_mcp_*")
endif()

# -----------------------------
# C++ Examples
# -----------------------------

option(FA_BUILD_EXAMPLES "Build C++ example programs" ON)

if (FA_BUILD_EXAMPLES AND FA_BUILD_HTTP)
    message(STATUS "Building C++ examples")

    # Hello World example - minimal example
    add_executable(hello_world examples/hello_world.cpp)
    target_include_directories(hello_world PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(hello_world PRIVATE fasterapi_http)
    set_target_properties(hello_world PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )

    # Simple test without docs
    add_executable(simple_test examples/simple_test.cpp)
    target_include_directories(simple_test PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(simple_test PRIVATE fasterapi_http)
    set_target_properties(simple_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )

    # Comprehensive demo - showcases all features
    add_executable(cpp_app_demo examples/cpp_app_demo.cpp)
    target_include_directories(cpp_app_demo PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(cpp_app_demo PRIVATE fasterapi_http)
    set_target_properties(cpp_app_demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )

    # Pure C++ server - no Python/ZMQ, maximum performance
    add_executable(pure_cpp_server examples/pure_cpp_server.cpp)
    target_include_directories(pure_cpp_server PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(pure_cpp_server PRIVATE fasterapi_http)
    set_target_properties(pure_cpp_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )

    # Pure C++ WebSocket test server - for E2E testing WebSocket in pure C++ mode
    add_executable(test_pure_cpp_websocket_server tests/test_pure_cpp_websocket_server.cpp)
    target_include_directories(test_pure_cpp_websocket_server PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_pure_cpp_websocket_server PRIVATE fasterapi_http)
    set_target_properties(test_pure_cpp_websocket_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    message(STATUS "C++ examples will be built at: ${CMAKE_BINARY_DIR}/examples/")
    message(STATUS "  - hello_world: Minimal 'Hello World' example")
    message(STATUS "  - cpp_app_demo: Comprehensive feature demonstration")
    message(STATUS "  - pure_cpp_server: Pure C++ server (no Python/ZMQ)")
endif()

# -----------------------------
# Cython Python Bindings
# -----------------------------

option(FA_BUILD_CYTHON "Build Cython Python bindings" ON)

if (FA_BUILD_CYTHON AND FA_BUILD_HTTP)
    message(STATUS "Building Cython Python bindings")

    # Find Cython
    find_program(CYTHON_EXECUTABLE cython)
    if (NOT CYTHON_EXECUTABLE)
        message(WARNING "Cython not found - Python bindings will not be built")
        message(WARNING "Install with: pip install cython")
        set(FA_BUILD_CYTHON OFF)
    else()
        message(STATUS "Cython found: ${CYTHON_EXECUTABLE}")
    endif()
endif()

if (FA_BUILD_CYTHON AND FA_BUILD_HTTP AND CYTHON_EXECUTABLE)
    # Function to add Cython extension module
    function(add_cython_extension name pyx_file)
        # Get absolute path for .pyx file
        get_filename_component(pyx_file_abs "${pyx_file}" ABSOLUTE)

        # Output C++ file
        set(c_file "${CMAKE_CURRENT_BINARY_DIR}/_cython/${name}.cpp")

        # Create output directory
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_cython")

        # Cythonize command
        add_custom_command(
            OUTPUT ${c_file}
            COMMAND ${CYTHON_EXECUTABLE}
            ARGS --cplus -3 ${pyx_file_abs} -o ${c_file}
                -I ${CMAKE_SOURCE_DIR}
            DEPENDS ${pyx_file}
            COMMENT "Cythonizing ${name}"
            VERBATIM
        )

        # Create Python extension module
        Python3_add_library(${name} MODULE ${c_file})

        # Include directories
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/cpp
            ${Python3_INCLUDE_DIRS}
        )

        # Link libraries
        target_link_libraries(${name} PRIVATE
            fasterapi_http
        )

        # Set output properties
        set_target_properties(${name} PROPERTIES
            PREFIX ""
            OUTPUT_NAME ${name}
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/fasterapi/_native"
            SUFFIX ".${Python3_SOABI}${CMAKE_SHARED_LIBRARY_SUFFIX}"
        )

        # Add to install target
        install(TARGETS ${name}
            LIBRARY DESTINATION fasterapi/_native
        )
    endfunction()

    # Add HTTP/2 Cython extension
    if (FA_ENABLE_HTTP2)
        add_cython_extension(http2 "${CMAKE_SOURCE_DIR}/fasterapi/_cython/http2.pyx")
        message(STATUS "HTTP/2 Cython extension will be built at: ${CMAKE_SOURCE_DIR}/fasterapi/_native/http2.*.so")
    endif()

    message(STATUS "Cython extensions will be built to: ${CMAKE_SOURCE_DIR}/fasterapi/_native/")
endif()

# -----------------------------
# Google Test Based Tests
# -----------------------------

if(FA_BUILD_GTEST AND TARGET gtest)
    message(STATUS "Building Google Test based tests")

    # Helper function to create gtest executable
    function(add_gtest_test name sources)
        add_executable(${name} ${sources})
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/cpp
        )
        target_link_libraries(${name} PRIVATE gtest gtest_main gmock)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/gtest"
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
        )
        # Enable exceptions for gtest (gtest requires them)
        target_compile_options(${name} PRIVATE -fexceptions)
        gtest_discover_tests(${name})
    endfunction()

    # Core module tests
    add_gtest_test(gtest_ring_buffer
        tests/core/ring_buffer_test.cpp
    )
    target_sources(gtest_ring_buffer PRIVATE
        src/cpp/core/ring_buffer.cpp
    )

    add_gtest_test(gtest_lockfree_queue
        tests/core/lockfree_queue_test.cpp
    )

    # Network layer tests
    add_gtest_test(gtest_event_loop
        tests/net/event_loop_test.cpp
    )
    target_sources(gtest_event_loop PRIVATE
        src/cpp/net/event_loop.cpp
        $<$<PLATFORM_ID:Darwin>:src/cpp/net/event_loop_kqueue.cpp>
        $<$<PLATFORM_ID:Linux>:src/cpp/net/event_loop_epoll.cpp>
    )

    add_gtest_test(gtest_tcp_socket
        tests/net/tcp_socket_test.cpp
    )
    target_sources(gtest_tcp_socket PRIVATE
        src/cpp/net/event_loop.cpp
        src/cpp/net/tcp_socket.cpp
        $<$<PLATFORM_ID:Darwin>:src/cpp/net/event_loop_kqueue.cpp>
        $<$<PLATFORM_ID:Linux>:src/cpp/net/event_loop_epoll.cpp>
    )

    # HTTP stack tests
    add_gtest_test(gtest_router
        tests/http/router_test.cpp
    )
    target_sources(gtest_router PRIVATE
        src/cpp/http/router.cpp
    )

    add_gtest_test(gtest_http1_parser
        tests/http/http1_parser_test.cpp
    )
    target_sources(gtest_http1_parser PRIVATE
        src/cpp/http/http1_parser.cpp
    )

    add_gtest_test(gtest_http2_frame
        tests/http/http2_frame_test.cpp
    )
    target_sources(gtest_http2_frame PRIVATE
        src/cpp/http/http2_frame.cpp
    )

    # WebSocket tests
    add_gtest_test(gtest_websocket
        tests/gtest/gtest_websocket.cpp
    )
    target_sources(gtest_websocket PRIVATE
        src/cpp/http/websocket.cpp
        src/cpp/http/websocket_parser.cpp
    )
    target_link_libraries(gtest_websocket PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    # WebTransport tests
    add_gtest_test(gtest_webtransport
        tests/test_webtransport.cpp
    )
    target_sources(gtest_webtransport PRIVATE
        src/cpp/http/webtransport_connection.cpp
    )
    target_link_libraries(gtest_webtransport PRIVATE fasterapi_http OpenSSL::SSL OpenSSL::Crypto)

    message(STATUS "Google Test tests will be built at: ${CMAKE_BINARY_DIR}/tests/gtest/")
endif()

# -----------------------------
# Google Benchmark Based Benchmarks
# -----------------------------

if(FA_BUILD_GBENCH AND TARGET benchmark)
    message(STATUS "Building Google Benchmark based benchmarks")

    # Helper function to create gbench executable
    function(add_gbench name sources)
        add_executable(${name} ${sources})
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/src/cpp
        )
        target_link_libraries(${name} PRIVATE benchmark benchmark_main)
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks/gbench"
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
        )
    endfunction()

    # Core module benchmarks
    add_gbench(gbench_ring_buffer benchmarks/micro/ring_buffer_bench.cpp)
    target_sources(gbench_ring_buffer PRIVATE
        src/cpp/core/ring_buffer.cpp
    )

    add_gbench(gbench_queue benchmarks/micro/queue_bench.cpp)

    message(STATUS "Google Benchmark benchmarks will be built at: ${CMAKE_BINARY_DIR}/benchmarks/gbench/")
endif()
