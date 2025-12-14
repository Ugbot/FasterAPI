"""
Static file serving for FasterAPI.

Compatible with Starlette's StaticFiles interface.

Usage:
    from fasterapi.staticfiles import StaticFiles

    app.mount("/static", StaticFiles(directory="static"))
"""

import mimetypes
import os
import stat
from email.utils import formatdate
from typing import Any, Callable, Dict, List, Optional
from urllib.parse import unquote


class StaticFiles:
    """
    Serve static files from a directory.

    Usage:
        from fasterapi import FastAPI
        from fasterapi.staticfiles import StaticFiles

        app = FastAPI()
        app.mount("/static", StaticFiles(directory="static"))

        # Or with HTML mode (serves index.html for directories)
        app.mount("/", StaticFiles(directory="dist", html=True))
    """

    def __init__(
        self,
        directory: str = None,
        packages: List[str] = None,
        html: bool = False,
        check_dir: bool = True,
    ):
        """
        Initialize static file handler.

        Args:
            directory: Directory to serve files from
            packages: List of package names to serve static files from
            html: If True, serve index.html for directory requests
            check_dir: If True, verify directory exists on init
        """
        self.directory = directory
        self.packages = packages or []
        self.html = html

        if directory and check_dir:
            if not os.path.isdir(directory):
                raise RuntimeError(f"Directory '{directory}' does not exist")

        # Initialize mimetypes
        mimetypes.init()
        self._add_common_types()

    def _add_common_types(self):
        """Add common MIME types that might be missing."""
        mimetypes.add_type("text/javascript", ".js")
        mimetypes.add_type("text/javascript", ".mjs")
        mimetypes.add_type("application/json", ".json")
        mimetypes.add_type("image/svg+xml", ".svg")
        mimetypes.add_type("font/woff", ".woff")
        mimetypes.add_type("font/woff2", ".woff2")
        mimetypes.add_type("application/wasm", ".wasm")
        mimetypes.add_type("text/css", ".css")
        mimetypes.add_type("text/html", ".html")
        mimetypes.add_type("text/plain", ".txt")
        mimetypes.add_type("image/png", ".png")
        mimetypes.add_type("image/jpeg", ".jpg")
        mimetypes.add_type("image/jpeg", ".jpeg")
        mimetypes.add_type("image/gif", ".gif")
        mimetypes.add_type("image/webp", ".webp")
        mimetypes.add_type("image/x-icon", ".ico")

    async def __call__(
        self, scope: Dict[str, Any], receive: Callable, send: Callable
    ) -> None:
        """ASGI interface."""
        if scope["type"] != "http":
            raise TypeError("StaticFiles only handles HTTP requests")

        path = scope.get("path", "/")
        method = scope.get("method", "GET")

        # Only handle GET and HEAD
        if method not in ("GET", "HEAD"):
            await self._send_response(send, 405, b"Method Not Allowed")
            return

        # Get file path
        file_path = self._get_file_path(path)

        if file_path is None:
            await self._send_response(send, 404, b"Not Found")
            return

        # Security check - prevent directory traversal
        real_path = os.path.realpath(file_path)
        real_dir = os.path.realpath(self.directory)
        if not real_path.startswith(real_dir + os.sep) and real_path != real_dir:
            await self._send_response(send, 403, b"Forbidden")
            return

        # Check if file exists
        if not os.path.isfile(real_path):
            if self.html and os.path.isdir(real_path):
                # Try index.html
                index_path = os.path.join(real_path, "index.html")
                if os.path.isfile(index_path):
                    real_path = index_path
                else:
                    await self._send_response(send, 404, b"Not Found")
                    return
            elif self.html:
                # SPA fallback - serve root index.html
                index_path = os.path.join(self.directory, "index.html")
                if os.path.isfile(index_path):
                    real_path = index_path
                else:
                    await self._send_response(send, 404, b"Not Found")
                    return
            else:
                await self._send_response(send, 404, b"Not Found")
                return

        # Serve the file
        await self._serve_file(scope, send, real_path, method == "HEAD")

    def _get_file_path(self, path: str) -> Optional[str]:
        """Get the file path for a request path."""
        # Decode URL encoding
        path = unquote(path)

        # Remove leading slash
        if path.startswith("/"):
            path = path[1:]

        if self.directory:
            return os.path.join(self.directory, path)

        return None

    async def _serve_file(
        self, scope: Dict, send: Callable, file_path: str, head_only: bool = False
    ) -> None:
        """Serve a file."""
        try:
            stat_result = os.stat(file_path)
        except OSError:
            await self._send_response(send, 404, b"Not Found")
            return

        # Get content type
        content_type, _ = mimetypes.guess_type(file_path)
        if content_type is None:
            content_type = "application/octet-stream"

        # Build headers
        headers = [
            (b"content-type", content_type.encode()),
            (b"content-length", str(stat_result.st_size).encode()),
            (b"last-modified", formatdate(stat_result.st_mtime, usegmt=True).encode()),
            (b"etag", f'"{stat_result.st_mtime}-{stat_result.st_size}"'.encode()),
            (b"accept-ranges", b"bytes"),
        ]

        # Check for conditional request (If-None-Match)
        request_headers = dict(scope.get("headers", []))
        if_none_match = request_headers.get(b"if-none-match", b"").decode()
        etag = f'"{stat_result.st_mtime}-{stat_result.st_size}"'

        if if_none_match == etag:
            # Not modified
            await send(
                {
                    "type": "http.response.start",
                    "status": 304,
                    "headers": [(b"etag", etag.encode())],
                }
            )
            await send({"type": "http.response.body", "body": b""})
            return

        # Send response headers
        await send(
            {
                "type": "http.response.start",
                "status": 200,
                "headers": headers,
            }
        )

        if head_only:
            await send({"type": "http.response.body", "body": b""})
            return

        # Stream file content
        chunk_size = 64 * 1024  # 64KB chunks

        with open(file_path, "rb") as f:
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                await send(
                    {
                        "type": "http.response.body",
                        "body": chunk,
                        "more_body": True,
                    }
                )

        await send({"type": "http.response.body", "body": b"", "more_body": False})

    async def _send_response(
        self, send: Callable, status: int, body: bytes, content_type: str = "text/plain"
    ) -> None:
        """Send a simple response."""
        await send(
            {
                "type": "http.response.start",
                "status": status,
                "headers": [
                    (b"content-type", content_type.encode()),
                    (b"content-length", str(len(body)).encode()),
                ],
            }
        )
        await send({"type": "http.response.body", "body": body})


__all__ = ["StaticFiles"]
