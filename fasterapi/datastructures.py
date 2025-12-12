"""
FastAPI-compatible data structures for FasterAPI.

Provides UploadFile and other data structures for handling file uploads
and form data.

Usage:
    from fasterapi import UploadFile, File

    @app.post("/upload")
    async def upload_file(file: UploadFile = File(...)):
        contents = await file.read()
        return {"filename": file.filename, "size": len(contents)}
"""

import asyncio
import os
from tempfile import SpooledTemporaryFile
from typing import Any, BinaryIO, Dict, Optional, Union

# Default spool size before writing to disk (1MB)
DEFAULT_SPOOL_SIZE = 1024 * 1024


class UploadFile:
    """
    Represents an uploaded file from a multipart form.

    Provides both sync and async interfaces for reading file contents.
    Uses SpooledTemporaryFile for memory-efficient handling of large files.

    Attributes:
        filename: Original filename from the client
        content_type: MIME type of the file
        file: SpooledTemporaryFile containing the data
        size: File size in bytes (if known)
        headers: Additional headers from the multipart part

    Usage:
        @app.post("/upload")
        async def upload_file(file: UploadFile = File(...)):
            # Read all contents
            contents = await file.read()

            # Or read in chunks
            while chunk := await file.read(1024):
                process(chunk)

            # Seek back to beginning
            await file.seek(0)

            # Close when done
            await file.close()

            return {"filename": file.filename, "size": file.size}
    """

    __slots__ = (
        "filename",
        "content_type",
        "file",
        "size",
        "headers",
        "_in_memory",
        "spool_max_size",
    )

    def __init__(
        self,
        filename: str,
        file: Optional[BinaryIO] = None,
        content_type: str = "application/octet-stream",
        size: Optional[int] = None,
        headers: Optional[Dict[str, str]] = None,
        spool_max_size: int = DEFAULT_SPOOL_SIZE,
    ):
        """
        Initialize UploadFile.

        Args:
            filename: Original filename
            file: File-like object (if None, creates SpooledTemporaryFile)
            content_type: MIME type of the file
            size: File size in bytes
            headers: Additional headers from multipart
            spool_max_size: Max size in memory before spilling to disk
        """
        self.filename = filename
        self.content_type = content_type
        self.size = size
        self.headers = headers or {}
        self.spool_max_size = spool_max_size
        self._in_memory = True

        if file is None:
            self.file = SpooledTemporaryFile(
                max_size=spool_max_size,
                mode="w+b",
            )
        else:
            self.file = file

    @classmethod
    def from_bytes(
        cls,
        data: bytes,
        filename: str,
        content_type: str = "application/octet-stream",
        headers: Optional[Dict[str, str]] = None,
    ) -> "UploadFile":
        """
        Create UploadFile from bytes.

        Args:
            data: File contents as bytes
            filename: Filename
            content_type: MIME type
            headers: Additional headers

        Returns:
            UploadFile instance
        """
        upload = cls(
            filename=filename,
            content_type=content_type,
            size=len(data),
            headers=headers,
        )
        upload.file.write(data)
        upload.file.seek(0)
        return upload

    # Async methods (primary API for FastAPI compatibility)

    async def read(self, size: int = -1) -> bytes:
        """
        Read file contents asynchronously.

        Args:
            size: Number of bytes to read (-1 for all)

        Returns:
            File contents as bytes
        """
        return await asyncio.to_thread(self.file.read, size)

    async def write(self, data: bytes) -> int:
        """
        Write data to the file asynchronously.

        Args:
            data: Data to write

        Returns:
            Number of bytes written
        """
        result = await asyncio.to_thread(self.file.write, data)
        # Update size
        if self.size is not None:
            self.size += len(data)
        else:
            pos = self.file.tell()
            self.file.seek(0, 2)  # Seek to end
            self.size = self.file.tell()
            self.file.seek(pos)
        return result

    async def seek(self, offset: int, whence: int = 0) -> int:
        """
        Seek to position in file asynchronously.

        Args:
            offset: Position offset
            whence: Reference point (0=start, 1=current, 2=end)

        Returns:
            New position
        """
        return await asyncio.to_thread(self.file.seek, offset, whence)

    async def close(self) -> None:
        """Close the file asynchronously."""
        await asyncio.to_thread(self.file.close)

    # Sync methods (for non-async contexts)

    def read_sync(self, size: int = -1) -> bytes:
        """Read file contents synchronously."""
        return self.file.read(size)

    def write_sync(self, data: bytes) -> int:
        """Write data synchronously."""
        return self.file.write(data)

    def seek_sync(self, offset: int, whence: int = 0) -> int:
        """Seek synchronously."""
        return self.file.seek(offset, whence)

    def close_sync(self) -> None:
        """Close file synchronously."""
        self.file.close()

    # Context manager support

    async def __aenter__(self) -> "UploadFile":
        """Async context manager entry."""
        return self

    async def __aexit__(
        self,
        exc_type: Any,
        exc_val: Any,
        exc_tb: Any,
    ) -> None:
        """Async context manager exit."""
        await self.close()

    def __enter__(self) -> "UploadFile":
        """Sync context manager entry."""
        return self

    def __exit__(
        self,
        exc_type: Any,
        exc_val: Any,
        exc_tb: Any,
    ) -> None:
        """Sync context manager exit."""
        self.close_sync()

    def __repr__(self) -> str:
        return (
            f"UploadFile("
            f"filename={self.filename!r}, "
            f"content_type={self.content_type!r}, "
            f"size={self.size})"
        )


class FormData(dict):
    """
    Dictionary-like object for form data.

    Stores form fields and uploaded files from multipart/form-data
    or application/x-www-form-urlencoded requests.

    Usage:
        form = await request.form()
        username = form["username"]  # String field
        file = form["file"]  # UploadFile
    """

    def close(self) -> None:
        """Close all UploadFile objects in the form data."""
        for value in self.values():
            if isinstance(value, UploadFile):
                value.close_sync()
            elif isinstance(value, list):
                for item in value:
                    if isinstance(item, UploadFile):
                        item.close_sync()

    async def aclose(self) -> None:
        """Close all UploadFile objects asynchronously."""
        for value in self.values():
            if isinstance(value, UploadFile):
                await value.close()
            elif isinstance(value, list):
                for item in value:
                    if isinstance(item, UploadFile):
                        await item.close()

    def getlist(self, key: str) -> list:
        """
        Get all values for a key as a list.

        Useful for multiple file uploads or repeated form fields.

        Args:
            key: Field name

        Returns:
            List of values (empty if key not found)
        """
        value = self.get(key)
        if value is None:
            return []
        if isinstance(value, list):
            return value
        return [value]


class URL:
    """
    URL representation for request.url.

    Provides easy access to URL components.

    Usage:
        url = request.url
        print(url.path)
        print(url.query)
        print(url.scheme)
    """

    __slots__ = (
        "_url",
        "scheme",
        "netloc",
        "path",
        "query",
        "fragment",
        "username",
        "password",
        "hostname",
        "port",
    )

    def __init__(
        self,
        url: str = "",
        *,
        scheme: str = "http",
        netloc: str = "",
        path: str = "/",
        query: str = "",
        fragment: str = "",
    ):
        """
        Initialize URL.

        Args:
            url: Full URL string (if provided, components are parsed from it)
            scheme: URL scheme (http, https)
            netloc: Network location (host:port)
            path: URL path
            query: Query string (without ?)
            fragment: Fragment (without #)
        """
        if url:
            from urllib.parse import urlparse

            parsed = urlparse(url)
            self.scheme = parsed.scheme or scheme
            self.netloc = parsed.netloc or netloc
            self.path = parsed.path or path
            self.query = parsed.query or query
            self.fragment = parsed.fragment or fragment
            self.username = parsed.username
            self.password = parsed.password
            self.hostname = parsed.hostname
            self.port = parsed.port
        else:
            self.scheme = scheme
            self.netloc = netloc
            self.path = path
            self.query = query
            self.fragment = fragment
            self.username = None
            self.password = None
            self.hostname = netloc.split(":")[0] if netloc else None
            self.port = int(netloc.split(":")[1]) if ":" in netloc else None

        self._url = url

    @property
    def components(self) -> tuple:
        """Get URL components as a tuple."""
        return (self.scheme, self.netloc, self.path, self.query, self.fragment)

    def replace(self, **kwargs) -> "URL":
        """Create a new URL with replaced components."""
        return URL(
            scheme=kwargs.get("scheme", self.scheme),
            netloc=kwargs.get("netloc", self.netloc),
            path=kwargs.get("path", self.path),
            query=kwargs.get("query", self.query),
            fragment=kwargs.get("fragment", self.fragment),
        )

    def __str__(self) -> str:
        """Convert to string."""
        from urllib.parse import urlunsplit

        return urlunsplit(self.components)

    def __repr__(self) -> str:
        return f"URL({str(self)!r})"

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, URL):
            return self.components == other.components
        if isinstance(other, str):
            return str(self) == other
        return False

    def __hash__(self) -> int:
        return hash(self.components)


class Address:
    """
    Client address representation.

    Usage:
        client = request.client
        print(client.host)
        print(client.port)
    """

    __slots__ = ("host", "port")

    def __init__(self, host: str, port: Optional[int] = None):
        """
        Initialize Address.

        Args:
            host: IP address or hostname
            port: Port number
        """
        self.host = host
        self.port = port

    def __repr__(self) -> str:
        if self.port:
            return f"Address(host={self.host!r}, port={self.port})"
        return f"Address(host={self.host!r})"

    def __iter__(self):
        """Allow tuple unpacking: host, port = request.client"""
        yield self.host
        yield self.port


class State:
    """
    State object for storing request-scoped data.

    Used by middleware to pass data to handlers.

    Usage:
        @app.middleware("http")
        async def add_data(request, call_next):
            request.state.user = get_user(request)
            return await call_next(request)

        @app.get("/me")
        def get_me(request: Request):
            return {"user": request.state.user}
    """

    def __init__(self, state: Optional[Dict[str, Any]] = None):
        if state:
            for key, value in state.items():
                setattr(self, key, value)

    def __setattr__(self, name: str, value: Any) -> None:
        self.__dict__[name] = value

    def __getattr__(self, name: str) -> Any:
        try:
            return self.__dict__[name]
        except KeyError:
            message = f"'{type(self).__name__}' object has no attribute '{name}'"
            raise AttributeError(message)

    def __delattr__(self, name: str) -> None:
        del self.__dict__[name]

    def __contains__(self, name: str) -> bool:
        return name in self.__dict__

    def __repr__(self) -> str:
        return f"State({self.__dict__!r})"
