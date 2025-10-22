"""
WebSocket Python Wrapper

High-performance WebSocket connection backed by C++.
"""

from typing import Optional, Callable, Any, Union
from enum import Enum
import asyncio
from .bindings import get_lib


class OpCode(Enum):
    """WebSocket opcodes."""
    CONTINUATION = 0x0
    TEXT = 0x1
    BINARY = 0x2
    CLOSE = 0x8
    PING = 0x9
    PONG = 0xA


class CloseCode(Enum):
    """WebSocket close codes (RFC 6455)."""
    NORMAL = 1000
    GOING_AWAY = 1001
    PROTOCOL_ERROR = 1002
    UNSUPPORTED_DATA = 1003
    NO_STATUS = 1005
    ABNORMAL = 1006
    INVALID_PAYLOAD = 1007
    POLICY_VIOLATION = 1008
    MESSAGE_TOO_BIG = 1009
    MANDATORY_EXTENSION = 1010
    INTERNAL_ERROR = 1011
    TLS_HANDSHAKE = 1015


class WebSocket:
    """
    WebSocket connection handler backed by C++.
    
    Features:
    - Text and binary message support
    - Ping/pong handling
    - Automatic fragmentation
    - High performance (C++ core)
    
    Example:
        @app.websocket("/ws")
        async def websocket_endpoint(ws: WebSocket):
            await ws.send_text("Connected!")
            
            while ws.is_open():
                message = await ws.receive()
                await ws.send_text(f"Echo: {message}")
    """
    
    def __init__(self, native_handle: Optional[int] = None):
        """
        Create a WebSocket handler.
        
        Args:
            native_handle: C++ WebSocketConnection pointer (or None for testing)
        """
        self._handle = native_handle
        self._lib = get_lib() if native_handle else None
        self._receive_queue = asyncio.Queue()
        self._closed = False
        
        # Event callbacks
        self.on_open: Optional[Callable] = None
        self.on_close: Optional[Callable[[int, str], None]] = None
        self.on_error: Optional[Callable[[Exception], None]] = None
    
    def send_text(self, message: str) -> None:
        """
        Send text message.
        
        Args:
            message: Text message to send
            
        Raises:
            RuntimeError: If connection is closed or send fails
        """
        if not isinstance(message, str):
            raise TypeError("Message must be str")
        
        if not self._handle or not self._lib:
            raise RuntimeError("WebSocket not connected")
        
        result = self._lib.ws_send_text(self._handle, message.encode('utf-8'))
        if result != 0:
            raise RuntimeError(f"Failed to send text message: error {result}")
    
    def send_binary(self, data: bytes) -> None:
        """
        Send binary message.
        
        Args:
            data: Binary data to send
            
        Raises:
            RuntimeError: If connection is closed or send fails
        """
        if not isinstance(data, bytes):
            raise TypeError("Data must be bytes")
        
        if not self._handle or not self._lib:
            raise RuntimeError("WebSocket not connected")
        
        # Convert to ctypes array
        import ctypes
        data_array = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        
        result = self._lib.ws_send_binary(self._handle, data_array, len(data))
        if result != 0:
            raise RuntimeError(f"Failed to send binary message: error {result}")
    
    def send(self, message: Union[str, bytes]) -> None:
        """
        Send message (text or binary).
        
        Args:
            message: Message to send (str for text, bytes for binary)
        """
        if isinstance(message, str):
            self.send_text(message)
        elif isinstance(message, bytes):
            self.send_binary(message)
        else:
            raise TypeError("Message must be str or bytes")
    
    async def send_text_async(self, message: str) -> None:
        """Async version of send_text."""
        await asyncio.get_event_loop().run_in_executor(None, self.send_text, message)
    
    async def send_binary_async(self, data: bytes) -> None:
        """Async version of send_binary."""
        await asyncio.get_event_loop().run_in_executor(None, self.send_binary, data)
    
    async def send_async(self, message: Union[str, bytes]) -> None:
        """Async version of send."""
        if isinstance(message, str):
            await self.send_text_async(message)
        else:
            await self.send_binary_async(message)
    
    def ping(self, data: bytes = b'') -> None:
        """
        Send ping frame.
        
        Args:
            data: Optional ping data
        """
        if not self._handle or not self._lib:
            raise RuntimeError("WebSocket not connected")
        
        import ctypes
        if data:
            data_array = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
            self._lib.ws_send_ping(self._handle, data_array, len(data))
        else:
            self._lib.ws_send_ping(self._handle, None, 0)
    
    def pong(self, data: bytes = b'') -> None:
        """
        Send pong frame.
        
        Args:
            data: Optional pong data
        """
        if not self._handle or not self._lib:
            raise RuntimeError("WebSocket not connected")
        
        import ctypes
        if data:
            data_array = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
            self._lib.ws_send_pong(self._handle, data_array, len(data))
        else:
            self._lib.ws_send_pong(self._handle, None, 0)
    
    def close(self, code: int = 1000, reason: str = '') -> None:
        """
        Close WebSocket connection.
        
        Args:
            code: Close code (default: 1000 = normal closure)
            reason: Optional close reason
        """
        if self._closed:
            return
        
        if self._handle and self._lib:
            reason_bytes = reason.encode('utf-8') if reason else None
            self._lib.ws_close(self._handle, code, reason_bytes)
        
        self._closed = True
        
        if self.on_close:
            self.on_close(code, reason)
    
    async def close_async(self, code: int = 1000, reason: str = '') -> None:
        """Async version of close."""
        await asyncio.get_event_loop().run_in_executor(None, self.close, code, reason)
    
    def is_open(self) -> bool:
        """Check if connection is open."""
        if self._closed:
            return False
        
        if self._handle and self._lib:
            return self._lib.ws_is_open(self._handle)
        
        return False
    
    async def receive(self) -> Union[str, bytes]:
        """
        Receive next message (async).
        
        Returns:
            Message as str (text) or bytes (binary)
            
        Raises:
            RuntimeError: If connection is closed
        """
        if not self.is_open():
            raise RuntimeError("WebSocket is closed")
        
        # Wait for message from queue
        message = await self._receive_queue.get()
        
        if message is None:
            raise RuntimeError("WebSocket closed")
        
        return message
    
    async def receive_text(self) -> str:
        """
        Receive text message (async).
        
        Returns:
            Text message as str
        """
        msg = await self.receive()
        if isinstance(msg, bytes):
            return msg.decode('utf-8')
        return msg
    
    async def receive_bytes(self) -> bytes:
        """
        Receive binary message (async).
        
        Returns:
            Binary message as bytes
        """
        msg = await self.receive()
        if isinstance(msg, str):
            return msg.encode('utf-8')
        return msg
    
    def _handle_message(self, message: Union[str, bytes]) -> None:
        """
        Internal: Handle incoming message from C++.
        
        Called by the server when a message is received.
        """
        self._receive_queue.put_nowait(message)
    
    def _handle_close_event(self, code: int, reason: str) -> None:
        """
        Internal: Handle close event from C++.
        """
        self._closed = True
        self._receive_queue.put_nowait(None)  # Signal close
        
        if self.on_close:
            self.on_close(code, reason)
    
    def _handle_error(self, error: str) -> None:
        """
        Internal: Handle error from C++.
        """
        if self.on_error:
            self.on_error(RuntimeError(error))
    
    @property
    def messages_sent(self) -> int:
        """Get number of messages sent."""
        if self._handle and self._lib:
            return self._lib.ws_messages_sent(self._handle)
        return 0
    
    @property
    def messages_received(self) -> int:
        """Get number of messages received."""
        if self._handle and self._lib:
            return self._lib.ws_messages_received(self._handle)
        return 0
    
    def __del__(self):
        """Cleanup on destruction."""
        if not self._closed:
            self.close()
        
        if self._handle and self._lib:
            self._lib.ws_destroy(self._handle)
    
    def __repr__(self) -> str:
        """String representation."""
        state = "open" if self.is_open() else "closed"
        return f"WebSocket(state={state}, sent={self.messages_sent}, received={self.messages_received})"


__all__ = ['WebSocket', 'OpCode', 'CloseCode']
