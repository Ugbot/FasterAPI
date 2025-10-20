"""
FasterAPI HTTP Server Module

High-performance HTTP server with multi-protocol support:
- HTTP/1.1 (uWebSockets)
- HTTP/2 (nghttp2 + ALPN)
- HTTP/3 (MsQuic, optional)
- WebSocket support
- zstd compression
- Per-core event loops
"""

from .server import Server
from .request import Request
from .response import Response
from .websocket import WebSocket
from .bindings import get_lib

__all__ = [
    'Server',
    'Request', 
    'Response',
    'WebSocket',
    'get_lib'
]