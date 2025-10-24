# distutils: language = c++
# cython: language_level=3

"""
Cython declarations for FasterAPI C++ HTTP/2 server

This file declares the C++ classes and functions that will be wrapped.
"""

from libcpp.string cimport string
from libcpp cimport bool
from libcpp.unordered_map cimport unordered_map

# Forward declarations
cdef extern from "src/cpp/http/http2_server.h" namespace "fasterapi::http":

    # HTTP/2 Server Configuration
    cdef cppclass Http2ServerConfig:
        unsigned short port
        string host
        unsigned short num_pinned_workers
        unsigned short num_pooled_workers
        unsigned short num_pooled_interpreters
        bool use_reuseport
        bool enable_tls

    # HTTP/2 Server Class
    cdef cppclass Http2Server:
        Http2Server(const Http2ServerConfig& config) except +
        int start() nogil
        void stop() nogil
        bool is_running() nogil

# Python callback bridge declarations
cdef extern from "src/cpp/http/python_callback_bridge.h":

    # Forward declare the class
    cdef cppclass PythonCallbackBridge:
        pass

    # Declare static methods at namespace level
    cdef void PythonCallbackBridge_initialize "PythonCallbackBridge::initialize"()

    cdef void PythonCallbackBridge_register_handler "PythonCallbackBridge::register_handler"(
        const string& method,
        const string& path,
        int handler_id,
        void* callable
    )

    cdef void PythonCallbackBridge_poll_registrations "PythonCallbackBridge::poll_registrations"()

    cdef void PythonCallbackBridge_cleanup "PythonCallbackBridge::cleanup"()

# C API declarations (for backward compatibility)
cdef extern from "src/cpp/http/http2_server.h":
    void* http2_server_create(unsigned short port, unsigned short num_workers)
    int http2_server_start(void* server)
    void http2_server_stop(void* server)
    void http2_server_destroy(void* server)
