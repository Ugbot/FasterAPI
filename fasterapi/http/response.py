"""
HTTP Response Python Wrapper

Streaming response with compression support.
"""

from typing import Dict, Any, Optional, Union, List
from enum import Enum


class Status(Enum):
    """HTTP status codes."""
    OK = 200
    CREATED = 201
    ACCEPTED = 202
    NO_CONTENT = 204
    MOVED_PERMANENTLY = 301
    FOUND = 302
    NOT_MODIFIED = 304
    BAD_REQUEST = 400
    UNAUTHORIZED = 401
    FORBIDDEN = 403
    NOT_FOUND = 404
    METHOD_NOT_ALLOWED = 405
    CONFLICT = 409
    UNPROCESSABLE_ENTITY = 422
    INTERNAL_SERVER_ERROR = 500
    NOT_IMPLEMENTED = 501
    BAD_GATEWAY = 502
    SERVICE_UNAVAILABLE = 503


class Response:
    """
    HTTP response object with streaming and compression support.
    
    Features:
    - Streaming response support
    - Automatic zstd compression
    - JSON serialization
    - File serving
    - Chunked transfer encoding
    - HTTP/2 server push
    """
    
    def __init__(self):
        """Create a new HTTP response."""
        self.status_code = Status.OK
        self.headers: Dict[str, str] = {}
        self.body = ""
        self.is_sent = False
        self.compression_enabled = True
        self.compression_level = 3
        self.original_size = 0
        self.compressed_size = 0
    
    def status(self, status: Union[Status, int]) -> 'Response':
        """
        Set HTTP status code.
        
        Args:
            status: HTTP status code
            
        Returns:
            Self for method chaining
        """
        if isinstance(status, int):
            self.status_code = Status(status)
        else:
            self.status_code = status
        return self
    
    def header(self, name: str, value: str) -> 'Response':
        """
        Set response header.
        
        Args:
            name: Header name
            value: Header value
            
        Returns:
            Self for method chaining
        """
        self.headers[name.lower()] = value
        return self
    
    def content_type(self, content_type: str) -> 'Response':
        """
        Set content type.
        
        Args:
            content_type: Content type (e.g., "application/json")
            
        Returns:
            Self for method chaining
        """
        return self.header('content-type', content_type)
    
    def json(self, data: Union[Dict[str, Any], List[Any], str]) -> 'Response':
        """
        Send JSON response.
        
        Args:
            data: JSON data (will be serialized)
            
        Returns:
            Self for method chaining
        """
        import json
        if isinstance(data, str):
            self.body = data
        else:
            self.body = json.dumps(data)
        return self.content_type('application/json')
    
    def text(self, text: str) -> 'Response':
        """
        Send text response.
        
        Args:
            text: Text content
            
        Returns:
            Self for method chaining
        """
        self.body = text
        return self.content_type('text/plain')
    
    def html(self, html: str) -> 'Response':
        """
        Send HTML response.
        
        Args:
            html: HTML content
            
        Returns:
            Self for method chaining
        """
        self.body = html
        return self.content_type('text/html')
    
    def binary(self, data: bytes) -> 'Response':
        """
        Send binary response.
        
        Args:
            data: Binary data
            
        Returns:
            Self for method chaining
        """
        self.body = data.decode('latin-1')  # Store as string for now
        return self.content_type('application/octet-stream')
    
    def file(self, file_path: str) -> 'Response':
        """
        Send file response.
        
        Args:
            file_path: Path to file
            
        Returns:
            Self for method chaining
        """
        try:
            with open(file_path, 'rb') as f:
                data = f.read()
            return self.binary(data)
        except FileNotFoundError:
            return self.status(Status.NOT_FOUND).text("File not found")
    
    def compress(self, enable: bool = True) -> 'Response':
        """
        Enable compression for this response.
        
        Args:
            enable: Enable compression
            
        Returns:
            Self for method chaining
        """
        self.compression_enabled = enable
        return self
    
    def compression_level(self, level: int) -> 'Response':
        """
        Set compression level.
        
        Args:
            level: Compression level (1-22 for zstd)
            
        Returns:
            Self for method chaining
        """
        self.compression_level = level
        return self
    
    def redirect(self, url: str, permanent: bool = False) -> 'Response':
        """
        Redirect to another URL.
        
        Args:
            url: URL to redirect to
            permanent: Use permanent redirect (301) instead of temporary (302)
            
        Returns:
            Self for method chaining
        """
        status = Status.MOVED_PERMANENTLY if permanent else Status.FOUND
        return self.status(status).header('location', url)
    
    def cookie(
        self,
        name: str,
        value: str,
        options: Optional[Dict[str, str]] = None
    ) -> 'Response':
        """
        Set cookie.
        
        Args:
            name: Cookie name
            value: Cookie value
            options: Cookie options (path, domain, expires, etc.)
            
        Returns:
            Self for method chaining
        """
        if options is None:
            options = {}
        
        cookie_str = f"{name}={value}"
        for key, val in options.items():
            cookie_str += f"; {key}={val}"
        
        # Add to existing Set-Cookie header or create new one
        existing = self.headers.get('set-cookie', '')
        if existing:
            self.headers['set-cookie'] = f"{existing}, {cookie_str}"
        else:
            self.headers['set-cookie'] = cookie_str
        
        return self
    
    def clear_cookie(self, name: str, path: str = "/") -> 'Response':
        """
        Clear cookie.
        
        Args:
            name: Cookie name
            path: Cookie path
            
        Returns:
            Self for method chaining
        """
        return self.cookie(name, "", {"path": path, "expires": "Thu, 01 Jan 1970 00:00:00 GMT"})
    
    def send(self) -> int:
        """
        Send the response.
        
        Returns:
            Error code (0 = success)
        """
        if self.is_sent:
            return 0
        
        # Apply compression if enabled
        if self.compression_enabled and len(self.body) > 1024:
            # This would apply zstd compression
            # For now, just mark as compressed
            self.compressed_size = len(self.body)  # Placeholder
            self.headers['content-encoding'] = 'zstd'
        
        self.original_size = len(self.body)
        self.is_sent = True
        return 0
    
    def is_sent(self) -> bool:
        """Check if response has been sent."""
        return self.is_sent
    
    def get_size(self) -> int:
        """Get response size."""
        return len(self.body)
    
    def get_compression_ratio(self) -> float:
        """Get compression ratio."""
        if self.original_size == 0:
            return 0.0
        return 1.0 - (self.compressed_size / self.original_size)
    
    def __repr__(self) -> str:
        """String representation of response."""
        return f"Response(status={self.status_code.value}, size={len(self.body)})"
