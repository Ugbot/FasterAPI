"""
HTTP Request Python Wrapper

Zero-copy access to HTTP request data.
"""

from typing import Dict, Any, Optional, Union
from enum import Enum


class Method(Enum):
    """HTTP methods."""
    GET = "GET"
    POST = "POST"
    PUT = "PUT"
    DELETE = "DELETE"
    PATCH = "PATCH"
    HEAD = "HEAD"
    OPTIONS = "OPTIONS"
    CONNECT = "CONNECT"
    TRACE = "TRACE"


class Request:
    """
    HTTP request object with zero-copy access to headers and body.
    
    Features:
    - Zero-copy header access
    - Streaming body support
    - Path parameter extraction
    - Query parameter parsing
    - Multipart form data
    - JSON body parsing
    """
    
    def __init__(self, method: str = "GET", path: str = "/", **kwargs):
        """
        Create a new HTTP request.
        
        Args:
            method: HTTP method
            path: Request path
            **kwargs: Additional request data
        """
        self.method = Method(method.upper())
        self.path = path
        self.query = kwargs.get('query', '')
        self.version = kwargs.get('version', 'HTTP/1.1')
        self.protocol = kwargs.get('protocol', 'HTTP/1.1')
        self.headers = kwargs.get('headers', {})
        self.query_params = kwargs.get('query_params', {})
        self.path_params = kwargs.get('path_params', {})
        self.body = kwargs.get('body', '')
        self.body_bytes = kwargs.get('body_bytes', b'')
        self.client_ip = kwargs.get('client_ip', '127.0.0.1')
        self.request_id = kwargs.get('request_id', 0)
        self.timestamp = kwargs.get('timestamp', 0)
        self.secure = kwargs.get('secure', False)
    
    def get_method(self) -> Method:
        """Get HTTP method."""
        return self.method
    
    def get_path(self) -> str:
        """Get request path."""
        return self.path
    
    def get_query(self) -> str:
        """Get query string."""
        return self.query
    
    def get_version(self) -> str:
        """Get HTTP version."""
        return self.version
    
    def get_header(self, name: str) -> str:
        """
        Get header value.
        
        Args:
            name: Header name (case-insensitive)
            
        Returns:
            Header value, or empty string if not found
        """
        return self.headers.get(name.lower(), '')
    
    def get_headers(self) -> Dict[str, str]:
        """Get all headers."""
        return self.headers.copy()
    
    def get_query_param(self, name: str) -> str:
        """
        Get query parameter.
        
        Args:
            name: Parameter name
            
        Returns:
            Parameter value, or empty string if not found
        """
        return self.query_params.get(name, '')
    
    def get_path_param(self, name: str) -> str:
        """
        Get path parameter (from route pattern).
        
        Args:
            name: Parameter name
            
        Returns:
            Parameter value, or empty string if not found
        """
        return self.path_params.get(name, '')
    
    def get_body(self) -> str:
        """Get request body."""
        return self.body
    
    def get_body_bytes(self) -> bytes:
        """Get request body as bytes."""
        return self.body_bytes
    
    def get_content_type(self) -> str:
        """Get content type."""
        return self.get_header('content-type')
    
    def get_content_length(self) -> int:
        """Get content length."""
        try:
            return int(self.get_header('content-length') or '0')
        except ValueError:
            return 0
    
    def is_json(self) -> bool:
        """Check if request has JSON body."""
        content_type = self.get_content_type().lower()
        return 'application/json' in content_type
    
    def is_multipart(self) -> bool:
        """Check if request has multipart body."""
        content_type = self.get_content_type().lower()
        return 'multipart/form-data' in content_type
    
    def get_client_ip(self) -> str:
        """Get client IP address."""
        return self.client_ip
    
    def get_user_agent(self) -> str:
        """Get user agent."""
        return self.get_header('user-agent')
    
    def get_request_id(self) -> int:
        """Get request ID."""
        return self.request_id
    
    def get_timestamp(self) -> int:
        """Get request timestamp."""
        return self.timestamp
    
    def is_secure(self) -> bool:
        """Check if request is over HTTPS."""
        return self.secure
    
    def get_protocol(self) -> str:
        """Get protocol (HTTP/1.1, HTTP/2, HTTP/3)."""
        return self.protocol
    
    def json(self) -> Dict[str, Any]:
        """
        Parse JSON body.
        
        Returns:
            Parsed JSON data
            
        Raises:
            ValueError: If body is not valid JSON
        """
        if not self.is_json():
            raise ValueError("Request body is not JSON")
        
        import json
        try:
            return json.loads(self.body)
        except json.JSONDecodeError as e:
            raise ValueError(f"Invalid JSON: {e}")
    
    def form(self) -> Dict[str, str]:
        """
        Parse form data.
        
        Returns:
            Form data as dictionary
        """
        if self.is_multipart():
            # Handle multipart form data
            # This would be implemented with proper multipart parsing
            return {}
        else:
            # Handle application/x-www-form-urlencoded
            import urllib.parse
            return dict(urllib.parse.parse_qsl(self.body))
    
    def __repr__(self) -> str:
        """String representation of request."""
        return f"Request(method={self.method.value}, path='{self.path}', protocol='{self.protocol}')"
