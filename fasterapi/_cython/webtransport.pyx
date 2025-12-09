# distutils: language = c++
# cython: language_level=3

"""
Cython wrapper for FasterAPI WebTransport

Provides high-performance Python bindings to the native WebTransport implementation
over HTTP/3 and QUIC (RFC 9297).

Features:
- Bidirectional streams (reliable, ordered)
- Unidirectional streams (reliable, ordered, one-way)
- Datagrams (unreliable, unordered)
- Zero-copy where possible
"""

from fasterapi._cython.webtransport cimport (
    ConnectionID, QUICConnection,
    WebTransportConnection, WebTransportState,
    WT_CONNECTING, WT_CONNECTED, WT_CLOSING, WT_CLOSED,
    Http3Connection, Http3ConnectionSettings, Http3ConnectionState
)
from libcpp.string cimport string
from libcpp.unordered_map cimport unordered_map
from libcpp cimport bool
from libc.stdint cimport uint8_t, uint32_t, uint64_t, int64_t
from libc.string cimport memcpy
from cpython.bytes cimport PyBytes_AS_STRING, PyBytes_GET_SIZE
import time


cdef class PyWebTransportSession:
    """
    Python wrapper for WebTransport Session

    WebTransport (RFC 9297) provides bidirectional streams and unreliable
    datagrams over HTTP/3. This is ideal for:
    - Real-time gaming
    - Live streaming
    - IoT telemetry
    - Any application needing multiple streams over a single connection

    Example:
        # Server-side (after HTTP/3 CONNECT handshake)
        session = PyWebTransportSession(is_server=True)
        session.accept()

        # Open bidirectional stream
        stream_id = session.open_stream()
        session.send_stream(stream_id, b"Hello from server!")

        # Send unreliable datagram
        session.send_datagram(b"Ping!")

        # Close when done
        session.close()
    """

    cdef QUICConnection* _quic_conn
    cdef WebTransportConnection* _wt_conn
    cdef bool _owns_quic
    cdef bool _is_server
    cdef dict _stream_callbacks
    cdef list _datagram_callbacks
    cdef object _close_callback

    def __cinit__(self, bool is_server=True, uint64_t local_cid_seed=0, uint64_t peer_cid_seed=0):
        """
        Create a new WebTransport session.

        Args:
            is_server: True if this is a server-side session
            local_cid_seed: Seed for local connection ID (0 = auto-generate)
            peer_cid_seed: Seed for peer connection ID (0 = auto-generate)
        """
        self._quic_conn = NULL
        self._wt_conn = NULL
        self._owns_quic = True
        self._is_server = is_server
        self._stream_callbacks = {}
        self._datagram_callbacks = []
        self._close_callback = None

        # Generate connection IDs
        cdef ConnectionID local_cid
        cdef ConnectionID peer_cid

        if local_cid_seed == 0:
            local_cid_seed = <uint64_t>id(self) ^ <uint64_t>(time.time_ns())
        if peer_cid_seed == 0:
            peer_cid_seed = local_cid_seed ^ 0xFFFFFFFFFFFFFFFF

        local_cid.length = 8
        memcpy(local_cid.data, &local_cid_seed, 8)

        peer_cid.length = 8
        memcpy(peer_cid.data, &peer_cid_seed, 8)

        # Create QUIC connection
        self._quic_conn = new QUICConnection(is_server, local_cid, peer_cid)
        if self._quic_conn == NULL:
            raise MemoryError("Failed to allocate QUICConnection")

        self._quic_conn.initialize()

        # Create WebTransport connection (non-owning mode)
        self._wt_conn = new WebTransportConnection(self._quic_conn, is_server)
        if self._wt_conn == NULL:
            del self._quic_conn
            self._quic_conn = NULL
            raise MemoryError("Failed to allocate WebTransportConnection")

    def __dealloc__(self):
        """Clean up native resources."""
        if self._wt_conn != NULL:
            del self._wt_conn
            self._wt_conn = NULL

        if self._owns_quic and self._quic_conn != NULL:
            del self._quic_conn
            self._quic_conn = NULL

    def initialize(self):
        """
        Initialize the WebTransport session.

        Returns:
            0 on success, -1 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef int result
        with nogil:
            result = self._wt_conn.initialize()
        return result

    def accept(self):
        """
        Accept incoming WebTransport connection (server-side).

        Call this after receiving a WebTransport CONNECT request
        and deciding to accept the connection.

        Returns:
            0 on success, -1 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef int result
        with nogil:
            result = self._wt_conn.accept()
        return result

    def connect(self, str url):
        """
        Connect to WebTransport endpoint (client-side).

        Args:
            url: WebTransport URL (e.g., "https://example.com/wt")

        Returns:
            0 on success, -1 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef bytes url_bytes = url.encode('utf-8')
        cdef const char* url_ptr = url_bytes
        cdef int result
        with nogil:
            result = self._wt_conn.connect(url_ptr)
        return result

    def close(self, uint64_t error_code=0, str reason=""):
        """
        Close the WebTransport session.

        Args:
            error_code: Application error code (0 = normal close)
            reason: Human-readable reason phrase
        """
        if self._wt_conn == NULL:
            return

        cdef bytes reason_bytes
        cdef const char* reason_ptr

        if reason:
            reason_bytes = reason.encode('utf-8')
            reason_ptr = reason_bytes
        else:
            reason_ptr = NULL

        with nogil:
            self._wt_conn.close(error_code, reason_ptr)

    # ==========================================================================
    # Bidirectional Streams
    # ==========================================================================

    def open_stream(self):
        """
        Open a new bidirectional stream.

        Returns:
            Stream ID on success, 0 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef uint64_t stream_id
        with nogil:
            stream_id = self._wt_conn.open_stream()
        return stream_id

    def send_stream(self, uint64_t stream_id, bytes data):
        """
        Send data on a bidirectional stream.

        Args:
            stream_id: Stream ID from open_stream()
            data: Bytes to send

        Returns:
            Number of bytes sent, or -1 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef const uint8_t* data_ptr = <const uint8_t*>PyBytes_AS_STRING(data)
        cdef size_t data_len = PyBytes_GET_SIZE(data)
        cdef int64_t result

        with nogil:
            result = self._wt_conn.send_stream(stream_id, data_ptr, data_len)
        return result

    def close_stream(self, uint64_t stream_id):
        """
        Close a bidirectional stream.

        Args:
            stream_id: Stream ID to close

        Returns:
            0 on success, -1 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef int result
        with nogil:
            result = self._wt_conn.close_stream(stream_id)
        return result

    # ==========================================================================
    # Unidirectional Streams
    # ==========================================================================

    def open_unidirectional_stream(self):
        """
        Open a new unidirectional (send-only) stream.

        Returns:
            Stream ID on success, 0 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef uint64_t stream_id
        with nogil:
            stream_id = self._wt_conn.open_unidirectional_stream()
        return stream_id

    def send_unidirectional(self, uint64_t stream_id, bytes data):
        """
        Send data on a unidirectional stream.

        Args:
            stream_id: Stream ID from open_unidirectional_stream()
            data: Bytes to send

        Returns:
            Number of bytes sent, or -1 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef const uint8_t* data_ptr = <const uint8_t*>PyBytes_AS_STRING(data)
        cdef size_t data_len = PyBytes_GET_SIZE(data)
        cdef int64_t result

        with nogil:
            result = self._wt_conn.send_unidirectional(stream_id, data_ptr, data_len)
        return result

    def close_unidirectional_stream(self, uint64_t stream_id):
        """
        Close a unidirectional stream.

        Args:
            stream_id: Stream ID to close

        Returns:
            0 on success, -1 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef int result
        with nogil:
            result = self._wt_conn.close_unidirectional_stream(stream_id)
        return result

    # ==========================================================================
    # Datagrams
    # ==========================================================================

    def send_datagram(self, bytes data):
        """
        Send an unreliable datagram.

        Datagrams may be dropped or reordered. Use for latency-sensitive
        data where occasional loss is acceptable (e.g., game state updates).

        Args:
            data: Bytes to send (max ~1200 bytes due to MTU)

        Returns:
            0 on success, -1 on error (e.g., data too large)
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        cdef const uint8_t* data_ptr = <const uint8_t*>PyBytes_AS_STRING(data)
        cdef size_t data_len = PyBytes_GET_SIZE(data)
        cdef int result

        with nogil:
            result = self._wt_conn.send_datagram(data_ptr, data_len)
        return result

    # ==========================================================================
    # Packet I/O
    # ==========================================================================

    def process_datagram(self, bytes data, uint64_t now_us=0):
        """
        Process incoming UDP datagram (contains QUIC packet).

        Call this when data arrives from the network.

        Args:
            data: Raw UDP datagram bytes
            now_us: Current time in microseconds (0 = use system time)

        Returns:
            0 on success, -1 on error
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        if now_us == 0:
            now_us = <uint64_t>(time.time_ns() // 1000)

        cdef const uint8_t* data_ptr = <const uint8_t*>PyBytes_AS_STRING(data)
        cdef size_t data_len = PyBytes_GET_SIZE(data)
        cdef int result

        with nogil:
            result = self._wt_conn.process_datagram(data_ptr, data_len, now_us)
        return result

    def generate_datagrams(self, size_t capacity=4096, uint64_t now_us=0):
        """
        Generate outgoing UDP datagrams (contains QUIC packets).

        Call this periodically to get data to send over the network.

        Args:
            capacity: Maximum output buffer size
            now_us: Current time in microseconds (0 = use system time)

        Returns:
            Bytes to send over UDP, or empty bytes if nothing to send
        """
        if self._wt_conn == NULL:
            raise RuntimeError("WebTransport connection not initialized")

        if now_us == 0:
            now_us = <uint64_t>(time.time_ns() // 1000)

        cdef bytearray output = bytearray(capacity)
        cdef uint8_t* output_ptr = <uint8_t*>(<char*>output)
        cdef size_t generated

        with nogil:
            generated = self._wt_conn.generate_datagrams(output_ptr, capacity, now_us)

        if generated == 0:
            return b""
        return bytes(output[:generated])

    # ==========================================================================
    # State and Statistics
    # ==========================================================================

    @property
    def is_connected(self):
        """Check if session is connected."""
        if self._wt_conn == NULL:
            return False
        return self._wt_conn.is_connected()

    @property
    def is_closed(self):
        """Check if session is closed."""
        if self._wt_conn == NULL:
            return True
        return self._wt_conn.is_closed()

    @property
    def state(self):
        """
        Get current session state.

        Returns:
            String: "connecting", "connected", "closing", or "closed"
        """
        if self._wt_conn == NULL:
            return "closed"

        cdef WebTransportState s = self._wt_conn.state()
        if s == WT_CONNECTING:
            return "connecting"
        elif s == WT_CONNECTED:
            return "connected"
        elif s == WT_CLOSING:
            return "closing"
        else:
            return "closed"

    def get_stats(self):
        """
        Get session statistics.

        Returns:
            Dict with statistics:
            - streams_opened: Total streams opened
            - active_streams: Currently active streams
            - datagrams_sent: Total datagrams sent
            - datagrams_received: Total datagrams received
            - bytes_sent: Total bytes sent
            - bytes_received: Total bytes received
            - pending_datagrams: Datagrams queued for sending
        """
        if self._wt_conn == NULL:
            return {}

        cdef unordered_map[string, uint64_t] cpp_stats
        with nogil:
            cpp_stats = self._wt_conn.get_stats()

        # Convert C++ map to Python dict
        result = {}
        for pair in cpp_stats:
            result[pair.first.decode('utf-8')] = pair.second
        return result

    def __repr__(self):
        return f"PyWebTransportSession(state={self.state}, is_server={self._is_server})"


# =============================================================================
# HTTP/3 Connection Wrapper
# =============================================================================

cdef class PyHttp3Connection:
    """
    Python wrapper for HTTP/3 Connection.

    HTTP/3 is HTTP semantics over QUIC (RFC 9114). This provides:
    - Multiplexed streams without head-of-line blocking
    - Built-in encryption (TLS 1.3)
    - Fast connection establishment (0-RTT)

    This class is primarily for advanced use cases. Most users should
    use the higher-level server APIs.
    """

    cdef QUICConnection* _quic_conn
    cdef Http3Connection* _h3_conn
    cdef bool _is_server
    cdef Http3ConnectionSettings _settings

    def __cinit__(self, bool is_server=True,
                  uint32_t max_concurrent_streams=100,
                  uint32_t qpack_max_table_capacity=4096):
        """
        Create HTTP/3 connection.

        Args:
            is_server: True if server-side
            max_concurrent_streams: Maximum concurrent streams
            qpack_max_table_capacity: QPACK dynamic table capacity
        """
        self._quic_conn = NULL
        self._h3_conn = NULL
        self._is_server = is_server

        # Setup settings
        self._settings.max_header_list_size = 16384
        self._settings.qpack_max_table_capacity = qpack_max_table_capacity
        self._settings.qpack_blocked_streams = 100
        self._settings.max_concurrent_streams = max_concurrent_streams
        self._settings.connection_window_size = 16 * 1024 * 1024  # 16MB
        self._settings.stream_window_size = 1024 * 1024  # 1MB

        # Generate connection IDs
        cdef ConnectionID local_cid
        cdef ConnectionID peer_cid
        cdef uint64_t seed = <uint64_t>id(self) ^ <uint64_t>(time.time_ns())

        local_cid.length = 8
        memcpy(local_cid.data, &seed, 8)

        seed ^= 0xFFFFFFFFFFFFFFFF
        peer_cid.length = 8
        memcpy(peer_cid.data, &seed, 8)

        # Create HTTP/3 connection
        self._h3_conn = new Http3Connection(is_server, local_cid, peer_cid, self._settings)
        if self._h3_conn == NULL:
            raise MemoryError("Failed to allocate Http3Connection")

    def __dealloc__(self):
        if self._h3_conn != NULL:
            del self._h3_conn
            self._h3_conn = NULL

    def initialize(self):
        """Initialize the HTTP/3 connection."""
        if self._h3_conn == NULL:
            raise RuntimeError("HTTP/3 connection not initialized")

        cdef int result
        with nogil:
            result = self._h3_conn.initialize()
        return result

    def close(self, uint64_t error_code=0, str reason=""):
        """Close the connection."""
        if self._h3_conn == NULL:
            return

        cdef bytes reason_bytes
        cdef const char* reason_ptr

        if reason:
            reason_bytes = reason.encode('utf-8')
            reason_ptr = reason_bytes
        else:
            reason_ptr = NULL

        with nogil:
            self._h3_conn.close(error_code, reason_ptr)

    @property
    def is_active(self):
        """Check if connection is active."""
        if self._h3_conn == NULL:
            return False
        return self._h3_conn.is_active()

    @property
    def is_closed(self):
        """Check if connection is closed."""
        if self._h3_conn == NULL:
            return True
        return self._h3_conn.is_closed()

    @property
    def stream_count(self):
        """Get number of active streams."""
        if self._h3_conn == NULL:
            return 0
        return self._h3_conn.stream_count()

    @property
    def state(self):
        """Get connection state as string."""
        if self._h3_conn == NULL:
            return "closed"
        if self.is_active:
            return "active"
        elif self.is_closed:
            return "closed"
        else:
            return "initializing"

    def __repr__(self):
        state = "active" if self.is_active else ("closed" if self.is_closed else "unknown")
        return f"PyHttp3Connection(state={state}, streams={self.stream_count})"
