"""
WebSocket Python Wrapper

WebSocket connection handling.
"""

from typing import Optional, Callable, Any, Union
from enum import Enum


class OpCode(Enum):
    """WebSocket opcodes."""
    CONTINUATION = 0x0
    TEXT = 0x1
    BINARY = 0x2
    CLOSE = 0x8
    PING = 0x9
    PONG = 0xA


class WebSocket:
    """
    WebSocket connection handler.
    
    Features:
    - Text and binary message support
    - Ping/pong handling
    - Connection state management
    - Event callbacks
    """
    
    def __init__(self, handle: Any = None):
        """
        Create a new WebSocket handler.
        
        Args:
            handle: Native WebSocket handle
        """
        self.handle = handle
        self.is_open = False
        self.is_closing = False
        self.is_closed = False
        
        # Event callbacks
        self.on_open: Optional[Callable] = None
        self.on_message: Optional[Callable] = None
        self.on_close: Optional[Callable] = None
        self.on_error: Optional[Callable] = None
    
    def send_text(self, message: str) -> bool:
        """
        Send text message.
        
        Args:
            message: Text message to send
            
        Returns:
            True if sent successfully, False otherwise
        """
        if not self.is_open or self.is_closing:
            return False
        
        # This would send the message via the native WebSocket
        print(f"WebSocket sending text: {message}")
        return True
    
    def send_binary(self, data: bytes) -> bool:
        """
        Send binary message.
        
        Args:
            data: Binary data to send
            
        Returns:
            True if sent successfully, False otherwise
        """
        if not self.is_open or self.is_closing:
            return False
        
        # This would send the binary data via the native WebSocket
        print(f"WebSocket sending binary: {len(data)} bytes")
        return True
    
    def send(self, message: Union[str, bytes]) -> bool:
        """
        Send message (text or binary).
        
        Args:
            message: Message to send
            
        Returns:
            True if sent successfully, False otherwise
        """
        if isinstance(message, str):
            return self.send_text(message)
        elif isinstance(message, bytes):
            return self.send_binary(message)
        else:
            return False
    
    def ping(self, data: bytes = b'') -> bool:
        """
        Send ping frame.
        
        Args:
            data: Ping data
            
        Returns:
            True if sent successfully, False otherwise
        """
        if not self.is_open or self.is_closing:
            return False
        
        # This would send a ping frame
        print(f"WebSocket sending ping: {len(data)} bytes")
        return True
    
    def pong(self, data: bytes = b'') -> bool:
        """
        Send pong frame.
        
        Args:
            data: Pong data
            
        Returns:
            True if sent successfully, False otherwise
        """
        if not self.is_open or self.is_closing:
            return False
        
        # This would send a pong frame
        print(f"WebSocket sending pong: {len(data)} bytes")
        return True
    
    def close(self, code: int = 1000, reason: str = '') -> bool:
        """
        Close WebSocket connection.
        
        Args:
            code: Close code
            reason: Close reason
            
        Returns:
            True if close initiated successfully, False otherwise
        """
        if self.is_closed or self.is_closing:
            return False
        
        self.is_closing = True
        
        # This would send a close frame
        print(f"WebSocket closing: code={code}, reason='{reason}'")
        
        # Call close callback
        if self.on_close:
            self.on_close(code, reason)
        
        self.is_closed = True
        self.is_open = False
        return True
    
    def set_callbacks(
        self,
        on_open: Optional[Callable] = None,
        on_message: Optional[Callable] = None,
        on_close: Optional[Callable] = None,
        on_error: Optional[Callable] = None
    ) -> None:
        """
        Set event callbacks.
        
        Args:
            on_open: Called when connection opens
            on_message: Called when message is received
            on_close: Called when connection closes
            on_error: Called when error occurs
        """
        if on_open:
            self.on_open = on_open
        if on_message:
            self.on_message = on_message
        if on_close:
            self.on_close = on_close
        if on_error:
            self.on_error = on_error
    
    def handle_open(self) -> None:
        """Handle connection open event."""
        self.is_open = True
        if self.on_open:
            self.on_open()
    
    def handle_message(self, message: Union[str, bytes], opcode: OpCode) -> None:
        """
        Handle incoming message.
        
        Args:
            message: Received message
            opcode: Message opcode
        """
        if self.on_message:
            self.on_message(message, opcode)
    
    def handle_close(self, code: int, reason: str) -> None:
        """
        Handle connection close event.
        
        Args:
            code: Close code
            reason: Close reason
        """
        self.is_closed = True
        self.is_open = False
        if self.on_close:
            self.on_close(code, reason)
    
    def handle_error(self, error: Exception) -> None:
        """
        Handle error event.
        
        Args:
            error: Error that occurred
        """
        if self.on_error:
            self.on_error(error)
    
    def __repr__(self) -> str:
        """String representation of WebSocket."""
        state = "open" if self.is_open else "closed" if self.is_closed else "closing"
        return f"WebSocket(state={state})"
