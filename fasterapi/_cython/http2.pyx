# distutils: language = c++
# cython: language_level=3

"""
Cython wrapper for FasterAPI HTTP/2 Server

Provides high-performance Python bindings to the native HTTP/2 server.
"""

from fasterapi._cython.http cimport (
    Http2Server, Http2ServerConfig,
    PythonCallbackBridge,
    PythonCallbackBridge_initialize,
    PythonCallbackBridge_register_handler,
    PythonCallbackBridge_cleanup
)
from libcpp.string cimport string
from libcpp cimport bool
from cpython.ref cimport PyObject, Py_INCREF, Py_DECREF
import threading

# Helper function to register handler (avoids Python overhead)
cdef void _register_handler_internal(string method, string path, int handler_id, object handler):
    cdef void* handler_ptr = <void*><PyObject*>handler
    PythonCallbackBridge_register_handler(method, path, handler_id, handler_ptr)

cdef class PyHttp2Server:
    """
    Python wrapper for HTTP/2 Server

    High-performance HTTP/2 server using native event loop with:
    - Multi-threaded workers with SO_REUSEPORT
    - nghttp2 for HTTP/2 frame handling
    - Direct C++ integration via Cython (zero overhead)

    Example:
        server = PyHttp2Server(port=8080, num_workers=12)
        server.add_route("GET", "/", handler)
        server.start()  # Blocks until stop()
    """

    cdef Http2Server* _server
    cdef dict _handlers
    cdef object _stop_event
    cdef object _server_thread
    cdef unsigned short _port
    cdef unsigned short _num_pinned_workers
    cdef unsigned short _num_pooled_workers
    cdef unsigned short _num_pooled_interpreters

    def __cinit__(self,
                  unsigned short port=8080,
                  unsigned short num_pinned_workers=0,
                  unsigned short num_pooled_workers=0,
                  unsigned short num_pooled_interpreters=0):
        """
        Create HTTP/2 server with sub-interpreter configuration

        Args:
            port: Server port (default: 8080)
            num_pinned_workers: Workers with dedicated sub-interpreters (0 = auto = CPU count)
            num_pooled_workers: Additional workers sharing pooled interpreters (0 = none)
            num_pooled_interpreters: Size of shared interpreter pool (0 = auto = pooled_workers/2)
        """
        self._server = NULL
        self._handlers = {}
        self._stop_event = threading.Event()
        self._server_thread = None
        self._port = port
        self._num_pinned_workers = num_pinned_workers
        self._num_pooled_workers = num_pooled_workers
        self._num_pooled_interpreters = num_pooled_interpreters

        # Initialize Python callback bridge
        PythonCallbackBridge_initialize()

        # Create server config
        cdef Http2ServerConfig config
        config.port = port
        config.num_pinned_workers = num_pinned_workers
        config.num_pooled_workers = num_pooled_workers
        config.num_pooled_interpreters = num_pooled_interpreters
        config.host = b"0.0.0.0"
        config.use_reuseport = True
        config.enable_tls = False

        # Create server
        self._server = new Http2Server(config)

    def __dealloc__(self):
        """Cleanup on destruction"""
        # Stop server before cleanup
        if self._server != NULL:
            self._server.stop()
        # Note: Cython auto-deletes C++ objects allocated with 'new'

    def add_route(self, str method, str path, handler):
        """
        Register Python route handler

        Args:
            method: HTTP method (GET, POST, etc.)
            path: Route path (e.g., "/", "/api/users")
            handler: Python callable handler(request, response)

        The handler will be called with:
            - request: dict with 'method', 'path', 'headers', 'body'
            - response: dict to modify with 'status', 'content_type', 'body'

        Example:
            def hello(req, res):
                res['status'] = 200
                res['content_type'] = 'text/plain'
                res['body'] = 'Hello World'

            server.add_route("GET", "/", hello)
        """
        # Store handler reference (prevents garbage collection)
        handler_id = len(self._handlers)
        self._handlers[handler_id] = handler  # Keeps Python reference alive

        # Register with C++ bridge
        cdef string c_method = method.encode('utf-8')
        cdef string c_path = path.encode('utf-8')

        _register_handler_internal(c_method, c_path, handler_id, handler)

    def start(self, blocking=True):
        """
        Start HTTP/2 server

        Args:
            blocking: If True, blocks until stop() is called.
                     If False, starts in background thread.

        Returns:
            0 on success, -1 on error
        """
        if self._server == NULL:
            raise RuntimeError("Server not initialized")

        if blocking:
            # Start in current thread (blocks)
            return self._start_blocking()
        else:
            # Start in background thread
            return self._start_nonblocking()

    def _start_blocking(self):
        """Start server in current thread (blocks)"""
        cdef int result
        # CRITICAL: Release GIL before calling blocking C++ function
        # This allows worker threads to acquire GIL for Python callbacks
        with nogil:
            result = self._server.start()
        return result

    def _start_nonblocking(self):
        """Start server in background thread"""
        if self._server_thread is not None and self._server_thread.is_alive():
            return -1  # Already running

        self._stop_event.clear()

        def server_thread():
            try:
                self._start_blocking()
            except Exception as e:
                print(f"Server error: {e}")

        self._server_thread = threading.Thread(target=server_thread, daemon=True)
        self._server_thread.start()

        return 0

    def stop(self):
        """Stop HTTP/2 server"""
        if self._server != NULL:
            self._server.stop()
            self._stop_event.set()

        # Wait for background thread to finish
        if self._server_thread is not None:
            self._server_thread.join(timeout=5.0)
            self._server_thread = None

    def is_running(self):
        """Check if server is running"""
        if self._server == NULL:
            return False
        return self._server.is_running()

    @property
    def port(self):
        """Get server port"""
        return self._port

    @property
    def num_workers(self):
        """Get number of worker threads"""
        return self._num_pinned_workers + self._num_pooled_workers

    def __repr__(self):
        status = "running" if self.is_running() else "stopped"
        return f"PyHttp2Server(port={self._port}, workers={self.num_workers}, status={status})"


# Note: HTTP/2 subsystem initialized/cleaned up automatically in __cinit__/__dealloc__
