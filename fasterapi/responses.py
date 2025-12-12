"""
FastAPI-compatible response classes for FasterAPI.

Provides Response, JSONResponse, HTMLResponse, PlainTextResponse,
RedirectResponse, StreamingResponse, and FileResponse.
"""

import asyncio
import json
import mimetypes
import os
import stat
from email.utils import formatdate
from typing import (
    Any,
    AsyncIterable,
    Callable,
    Dict,
    Iterable,
    Mapping,
    Optional,
    Union,
)
from urllib.parse import quote


class Response:
    """
    Base HTTP response class.

    Usage:
        return Response(content="Hello", media_type="text/plain")
        return Response(content=b"binary data", status_code=201)
    """

    media_type: Optional[str] = None
    charset: str = "utf-8"

    def __init__(
        self,
        content: Any = None,
        status_code: int = 200,
        headers: Optional[Mapping[str, str]] = None,
        media_type: Optional[str] = None,
        background: Optional[Any] = None,
    ) -> None:
        self.status_code = status_code
        self.background = background

        # Initialize headers
        self.headers: Dict[str, str] = dict(headers) if headers else {}

        # Set media type
        if media_type is not None:
            self.media_type = media_type

        # Render body
        self.body = self.render(content)

        # Set content-type header if not already set
        if "content-type" not in {k.lower() for k in self.headers}:
            content_type = self.media_type
            if content_type and self.charset:
                content_type = f"{content_type}; charset={self.charset}"
            if content_type:
                self.headers["Content-Type"] = content_type

        # Set content-length if we have a body
        if self.body is not None and "content-length" not in {
            k.lower() for k in self.headers
        }:
            self.headers["Content-Length"] = str(len(self.body))

    def render(self, content: Any) -> bytes:
        """Render content to bytes."""
        if content is None:
            return b""
        if isinstance(content, bytes):
            return content
        return content.encode(self.charset)

    def set_cookie(
        self,
        key: str,
        value: str = "",
        max_age: Optional[int] = None,
        expires: Optional[Union[int, str]] = None,
        path: str = "/",
        domain: Optional[str] = None,
        secure: bool = False,
        httponly: bool = False,
        samesite: Optional[str] = "lax",
    ) -> None:
        """Set a cookie in the response."""
        cookie_parts = [f"{key}={quote(value)}"]

        if max_age is not None:
            cookie_parts.append(f"Max-Age={max_age}")
        if expires is not None:
            if isinstance(expires, int):
                expires = formatdate(expires, usegmt=True)
            cookie_parts.append(f"Expires={expires}")
        if path:
            cookie_parts.append(f"Path={path}")
        if domain:
            cookie_parts.append(f"Domain={domain}")
        if secure:
            cookie_parts.append("Secure")
        if httponly:
            cookie_parts.append("HttpOnly")
        if samesite:
            cookie_parts.append(f"SameSite={samesite}")

        cookie_header = "; ".join(cookie_parts)

        # Handle multiple Set-Cookie headers
        existing = self.headers.get("Set-Cookie")
        if existing:
            # Append with newline for multiple cookies
            self.headers["Set-Cookie"] = f"{existing}\n{cookie_header}"
        else:
            self.headers["Set-Cookie"] = cookie_header

    def delete_cookie(
        self,
        key: str,
        path: str = "/",
        domain: Optional[str] = None,
        secure: bool = False,
        httponly: bool = False,
        samesite: Optional[str] = "lax",
    ) -> None:
        """Delete a cookie by setting it to expire."""
        self.set_cookie(
            key,
            value="",
            max_age=0,
            expires=0,
            path=path,
            domain=domain,
            secure=secure,
            httponly=httponly,
            samesite=samesite,
        )


class HTMLResponse(Response):
    """Response class for HTML content."""

    media_type = "text/html"


class PlainTextResponse(Response):
    """Response class for plain text content."""

    media_type = "text/plain"


class JSONResponse(Response):
    """
    Response class for JSON content.

    Usage:
        return JSONResponse({"message": "Hello"})
        return JSONResponse(content={"error": "Not found"}, status_code=404)
    """

    media_type = "application/json"

    def __init__(
        self,
        content: Any = None,
        status_code: int = 200,
        headers: Optional[Mapping[str, str]] = None,
        media_type: Optional[str] = None,
        background: Optional[Any] = None,
    ) -> None:
        super().__init__(
            content=content,
            status_code=status_code,
            headers=headers,
            media_type=media_type,
            background=background,
        )

    def render(self, content: Any) -> bytes:
        """Render content as JSON."""
        if content is None:
            return b"null"
        return json.dumps(
            content,
            ensure_ascii=False,
            allow_nan=False,
            indent=None,
            separators=(",", ":"),
        ).encode("utf-8")


class RedirectResponse(Response):
    """
    Response class for HTTP redirects.

    Usage:
        return RedirectResponse(url="/new-location")
        return RedirectResponse("/permanent", status_code=301)
    """

    def __init__(
        self,
        url: str,
        status_code: int = 307,
        headers: Optional[Mapping[str, str]] = None,
        background: Optional[Any] = None,
    ) -> None:
        headers_dict = dict(headers) if headers else {}
        headers_dict["Location"] = quote(str(url), safe=":/%#?=@[]!$&'()*+,;")
        super().__init__(
            content=b"",
            status_code=status_code,
            headers=headers_dict,
            background=background,
        )


class StreamingResponse(Response):
    """
    Response class for streaming content.

    Usage:
        async def generate():
            for i in range(10):
                yield f"data: {i}\\n\\n"
                await asyncio.sleep(1)

        return StreamingResponse(generate(), media_type="text/event-stream")
    """

    body_iterator: Union[AsyncIterable[bytes], Iterable[bytes]]

    def __init__(
        self,
        content: Union[AsyncIterable[Any], Iterable[Any]],
        status_code: int = 200,
        headers: Optional[Mapping[str, str]] = None,
        media_type: Optional[str] = None,
        background: Optional[Any] = None,
    ) -> None:
        # Don't call super().__init__ as it would try to render the content
        self.status_code = status_code
        self.background = background
        self.headers = dict(headers) if headers else {}

        if media_type is not None:
            self.media_type = media_type

        # Set content-type header
        if "content-type" not in {k.lower() for k in self.headers}:
            content_type = self.media_type
            if content_type and self.charset:
                content_type = f"{content_type}; charset={self.charset}"
            if content_type:
                self.headers["Content-Type"] = content_type

        self.body_iterator = content
        self.body = None  # Streaming responses don't have a static body

    async def stream_response(self) -> AsyncIterable[bytes]:
        """Iterate over the response body."""
        if hasattr(self.body_iterator, "__aiter__"):
            # Async iterator
            async for chunk in self.body_iterator:
                if isinstance(chunk, str):
                    yield chunk.encode(self.charset)
                else:
                    yield chunk
        else:
            # Sync iterator - wrap in async
            for chunk in self.body_iterator:
                if isinstance(chunk, str):
                    yield chunk.encode(self.charset)
                else:
                    yield chunk


class FileResponse(Response):
    """
    Response class for file downloads.

    Usage:
        return FileResponse("path/to/file.pdf")
        return FileResponse("/data/image.png", filename="download.png")
    """

    chunk_size: int = 64 * 1024  # 64KB chunks

    def __init__(
        self,
        path: Union[str, os.PathLike],
        status_code: int = 200,
        headers: Optional[Mapping[str, str]] = None,
        media_type: Optional[str] = None,
        background: Optional[Any] = None,
        filename: Optional[str] = None,
        stat_result: Optional[os.stat_result] = None,
        method: Optional[str] = None,
        content_disposition_type: str = "attachment",
    ) -> None:
        self.path = path
        self.filename = filename
        self.status_code = status_code
        self.background = background
        self.method = method
        self.content_disposition_type = content_disposition_type

        # Initialize headers
        self.headers = dict(headers) if headers else {}

        # Get file stat
        if stat_result is None:
            stat_result = os.stat(path)
        self.stat_result = stat_result

        # Determine media type
        if media_type is None:
            media_type = self._guess_media_type()
        self.media_type = media_type

        # Set headers
        self._set_file_headers()

        # Body will be read lazily during streaming
        self.body = None

    def _guess_media_type(self) -> str:
        """Guess the media type from the filename."""
        if self.filename:
            name = self.filename
        else:
            name = os.path.basename(str(self.path))

        media_type, _ = mimetypes.guess_type(name)
        return media_type or "application/octet-stream"

    def _set_file_headers(self) -> None:
        """Set file-related headers."""
        # Content-Type
        if "content-type" not in {k.lower() for k in self.headers}:
            self.headers["Content-Type"] = self.media_type

        # Content-Length
        if "content-length" not in {k.lower() for k in self.headers}:
            self.headers["Content-Length"] = str(self.stat_result.st_size)

        # Last-Modified
        if "last-modified" not in {k.lower() for k in self.headers}:
            last_modified = formatdate(self.stat_result.st_mtime, usegmt=True)
            self.headers["Last-Modified"] = last_modified

        # ETag
        if "etag" not in {k.lower() for k in self.headers}:
            etag = f'"{self.stat_result.st_mtime}-{self.stat_result.st_size}"'
            self.headers["ETag"] = etag

        # Content-Disposition
        if self.filename and "content-disposition" not in {
            k.lower() for k in self.headers
        }:
            # RFC 5987 encoded filename
            encoded_filename = quote(self.filename)
            content_disposition = (
                f"{self.content_disposition_type}; "
                f'filename="{self.filename}"; '
                f"filename*=utf-8''{encoded_filename}"
            )
            self.headers["Content-Disposition"] = content_disposition

    async def stream_response(self) -> AsyncIterable[bytes]:
        """Stream the file content."""

        # Use thread pool for file I/O
        def read_file():
            with open(self.path, "rb") as f:
                while True:
                    chunk = f.read(self.chunk_size)
                    if not chunk:
                        break
                    yield chunk

        # Convert sync generator to async
        loop = asyncio.get_event_loop()

        with open(self.path, "rb") as f:
            while True:
                chunk = await loop.run_in_executor(None, f.read, self.chunk_size)
                if not chunk:
                    break
                yield chunk


class UJSONResponse(JSONResponse):
    """
    JSON response using ujson for faster serialization.
    Falls back to standard json if ujson is not available.
    """

    def render(self, content: Any) -> bytes:
        try:
            import ujson

            return ujson.dumps(content, ensure_ascii=False).encode("utf-8")
        except ImportError:
            return super().render(content)


class ORJSONResponse(JSONResponse):
    """
    JSON response using orjson for faster serialization.
    Falls back to standard json if orjson is not available.
    """

    def render(self, content: Any) -> bytes:
        try:
            import orjson

            return orjson.dumps(content)
        except ImportError:
            return super().render(content)


__all__ = [
    "Response",
    "HTMLResponse",
    "PlainTextResponse",
    "JSONResponse",
    "RedirectResponse",
    "StreamingResponse",
    "FileResponse",
    "UJSONResponse",
    "ORJSONResponse",
]
