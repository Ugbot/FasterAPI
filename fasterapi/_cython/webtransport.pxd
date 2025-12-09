# distutils: language = c++
# cython: language_level=3

"""
Cython declarations for FasterAPI C++ WebTransport

This file declares the C++ WebTransport classes for Python bindings.
"""

from libcpp.string cimport string
from libcpp cimport bool
from libcpp.unordered_map cimport unordered_map
from libcpp.functional cimport function
from libc.stdint cimport uint8_t, uint16_t, uint32_t, uint64_t, int64_t

# QUIC Connection ID
cdef extern from "src/cpp/http/quic/quic_connection.h" namespace "fasterapi::quic":
    cdef cppclass ConnectionID:
        uint8_t data[20]
        uint8_t length

    cdef cppclass QUICConnection:
        QUICConnection(bool is_server, const ConnectionID& local_cid, const ConnectionID& peer_cid) except +
        int initialize() nogil
        bool is_established() nogil
        bool is_closed() nogil

# WebTransport Connection
cdef extern from "src/cpp/http/webtransport_connection.h" namespace "fasterapi::http":

    # WebTransport Connection State
    cdef enum WebTransportState "fasterapi::http::WebTransportConnection::State":
        WT_CONNECTING "fasterapi::http::WebTransportConnection::State::CONNECTING"
        WT_CONNECTED "fasterapi::http::WebTransportConnection::State::CONNECTED"
        WT_CLOSING "fasterapi::http::WebTransportConnection::State::CLOSING"
        WT_CLOSED "fasterapi::http::WebTransportConnection::State::CLOSED"

    # WebTransport Connection Class
    cdef cppclass WebTransportConnection:
        # Constructors
        WebTransportConnection(QUICConnection* quic_conn, bool is_server) except +

        # Session management
        int initialize() nogil
        int accept() nogil
        int connect(const char* url) nogil
        void close(uint64_t error_code, const char* reason) nogil

        # Bidirectional streams
        uint64_t open_stream() nogil
        int64_t send_stream(uint64_t stream_id, const uint8_t* data, size_t length) nogil
        int close_stream(uint64_t stream_id) nogil

        # Unidirectional streams
        uint64_t open_unidirectional_stream() nogil
        int64_t send_unidirectional(uint64_t stream_id, const uint8_t* data, size_t length) nogil
        int close_unidirectional_stream(uint64_t stream_id) nogil

        # Datagrams
        int send_datagram(const uint8_t* data, size_t length) nogil

        # Packet processing
        int process_datagram(const uint8_t* data, size_t length, uint64_t now_us) nogil
        size_t generate_datagrams(uint8_t* output, size_t capacity, uint64_t now_us) nogil

        # State queries
        bool is_closed() nogil
        bool is_connected() nogil
        WebTransportState state() nogil

        # Statistics
        unordered_map[string, uint64_t] get_stats() nogil

# HTTP/3 Connection Settings
cdef extern from "src/cpp/http/http3_connection.h" namespace "fasterapi::http":

    cdef struct Http3ConnectionSettings:
        uint32_t max_header_list_size
        uint32_t qpack_max_table_capacity
        uint32_t qpack_blocked_streams
        uint32_t max_concurrent_streams
        uint32_t connection_window_size
        uint32_t stream_window_size

    # HTTP/3 Connection State
    cdef enum Http3ConnectionState "fasterapi::http::Http3ConnectionState":
        H3_IDLE "fasterapi::http::Http3ConnectionState::IDLE"
        H3_HANDSHAKE "fasterapi::http::Http3ConnectionState::HANDSHAKE"
        H3_ACTIVE "fasterapi::http::Http3ConnectionState::ACTIVE"
        H3_CLOSING "fasterapi::http::Http3ConnectionState::CLOSING"
        H3_CLOSED "fasterapi::http::Http3ConnectionState::CLOSED"

    cdef cppclass Http3Connection:
        Http3Connection(bool is_server, const ConnectionID& local_cid,
                        const ConnectionID& peer_cid, const Http3ConnectionSettings& settings) except +

        int initialize() nogil
        int process_datagram(const uint8_t* data, size_t length, uint64_t now_us) nogil
        size_t generate_datagrams(uint8_t* output, size_t capacity, uint64_t now_us) nogil

        bool is_closed() nogil
        bool is_active() nogil
        void close(uint64_t error_code, const char* reason) nogil

        Http3ConnectionState state() nogil
        size_t stream_count() nogil

        QUICConnection* quic_connection() nogil
