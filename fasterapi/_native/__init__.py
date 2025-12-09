# Native Cython extensions for FasterAPI
# These are high-performance Python bindings to C++ implementations

try:
    from .webtransport import PyWebTransportSession, PyHttp3Connection
except ImportError:
    # WebTransport module may not be built
    pass
