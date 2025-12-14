"""
StreamingResponse and Server-Sent Events (SSE) Tests

Tests for:
- StreamingResponse with sync generators
- StreamingResponse with async generators
- Server-Sent Events (SSE) format
- FileResponse streaming
- Response objects (HTMLResponse, PlainTextResponse, etc.)
"""

import asyncio
import os
import secrets
import tempfile
from typing import AsyncGenerator, Generator

from fasterapi import FastAPI
from fasterapi.responses import (
    FileResponse,
    HTMLResponse,
    JSONResponse,
    PlainTextResponse,
    RedirectResponse,
    Response,
    StreamingResponse,
)
from fasterapi.testclient import TestClient


class TestStreamingResponse:
    """Tests for StreamingResponse."""

    def test_sync_generator_streaming(self):
        """Test StreamingResponse with sync generator."""
        app = FastAPI()

        def generate_numbers() -> Generator[str, None, None]:
            for i in range(5):
                yield f"number: {i}\n"

        @app.get("/stream")
        def stream():
            return StreamingResponse(generate_numbers(), media_type="text/plain")

        client = TestClient(app)
        response = client.get("/stream")

        assert response.status_code == 200
        assert "text/plain" in response.headers.get("content-type", "")
        content = response.text
        for i in range(5):
            assert f"number: {i}" in content

    def test_async_generator_streaming(self):
        """Test StreamingResponse with async generator."""
        app = FastAPI()

        async def generate_data() -> AsyncGenerator[str, None]:
            for i in range(3):
                yield f"data-{i}\n"
                await asyncio.sleep(0.01)  # Small delay

        @app.get("/async-stream")
        async def async_stream():
            return StreamingResponse(generate_data(), media_type="text/plain")

        client = TestClient(app)
        response = client.get("/async-stream")

        assert response.status_code == 200
        content = response.text
        assert "data-0" in content
        assert "data-1" in content
        assert "data-2" in content

    def test_streaming_with_custom_headers(self):
        """Test StreamingResponse with custom headers."""
        app = FastAPI()

        def generate():
            yield b"chunk1"
            yield b"chunk2"

        @app.get("/custom-headers")
        def stream_with_headers():
            return StreamingResponse(
                generate(),
                media_type="application/octet-stream",
                headers={
                    "X-Custom-Header": "test-value",
                    "Cache-Control": "no-cache",
                },
            )

        client = TestClient(app)
        response = client.get("/custom-headers")

        assert response.status_code == 200
        assert response.headers.get("x-custom-header") == "test-value"
        assert response.headers.get("cache-control") == "no-cache"

    def test_bytes_streaming(self):
        """Test StreamingResponse with bytes chunks."""
        app = FastAPI()

        def generate_bytes() -> Generator[bytes, None, None]:
            for i in range(3):
                yield f"chunk{i}".encode()

        @app.get("/bytes-stream")
        def bytes_stream():
            return StreamingResponse(generate_bytes())

        client = TestClient(app)
        response = client.get("/bytes-stream")

        assert response.status_code == 200
        assert response.content == b"chunk0chunk1chunk2"


class TestServerSentEvents:
    """Tests for Server-Sent Events (SSE)."""

    def test_basic_sse(self):
        """Test basic SSE streaming."""
        app = FastAPI()

        async def event_generator() -> AsyncGenerator[str, None]:
            for i in range(3):
                yield f"data: message {i}\n\n"

        @app.get("/events")
        async def sse_events():
            return StreamingResponse(event_generator(), media_type="text/event-stream")

        client = TestClient(app)
        response = client.get("/events")

        assert response.status_code == 200
        assert "text/event-stream" in response.headers.get("content-type", "")
        content = response.text
        assert "data: message 0" in content
        assert "data: message 1" in content
        assert "data: message 2" in content

    def test_sse_with_event_types(self):
        """Test SSE with named event types."""
        app = FastAPI()

        async def typed_events() -> AsyncGenerator[str, None]:
            yield "event: start\ndata: Starting\n\n"
            yield "event: progress\ndata: 50%\n\n"
            yield "event: complete\ndata: Done\n\n"

        @app.get("/typed-events")
        async def sse_typed():
            return StreamingResponse(
                typed_events(),
                media_type="text/event-stream",
                headers={"Cache-Control": "no-cache"},
            )

        client = TestClient(app)
        response = client.get("/typed-events")

        assert response.status_code == 200
        content = response.text
        assert "event: start" in content
        assert "event: progress" in content
        assert "event: complete" in content

    def test_sse_with_ids(self):
        """Test SSE with event IDs."""
        app = FastAPI()

        async def events_with_ids() -> AsyncGenerator[str, None]:
            for i in range(3):
                yield f"id: {i}\ndata: Event {i}\n\n"

        @app.get("/events-with-ids")
        async def sse_with_ids():
            return StreamingResponse(events_with_ids(), media_type="text/event-stream")

        client = TestClient(app)
        response = client.get("/events-with-ids")

        content = response.text
        assert "id: 0" in content
        assert "id: 1" in content
        assert "id: 2" in content

    def test_sse_json_data(self):
        """Test SSE with JSON data."""
        import json

        app = FastAPI()

        async def json_events() -> AsyncGenerator[str, None]:
            for i in range(2):
                data = json.dumps({"index": i, "value": secrets.token_hex(4)})
                yield f"data: {data}\n\n"

        @app.get("/json-events")
        async def sse_json():
            return StreamingResponse(json_events(), media_type="text/event-stream")

        client = TestClient(app)
        response = client.get("/json-events")

        content = response.text
        # Parse the SSE data lines
        lines = [l for l in content.split("\n") if l.startswith("data:")]
        assert len(lines) == 2
        for line in lines:
            data = json.loads(line[6:])  # Strip "data: "
            assert "index" in data
            assert "value" in data


class TestFileResponse:
    """Tests for FileResponse."""

    def test_file_response(self):
        """Test basic file response."""
        # Create a temporary file
        content = f"Test file content: {secrets.token_hex(16)}"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
            f.write(content)
            temp_path = f.name

        try:
            app = FastAPI()

            @app.get("/download")
            def download():
                return FileResponse(temp_path)

            client = TestClient(app)
            response = client.get("/download")

            assert response.status_code == 200
            assert response.text == content
        finally:
            os.unlink(temp_path)

    def test_file_response_with_filename(self):
        """Test file response with custom filename."""
        content = b"Binary content " + secrets.token_bytes(16)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(content)
            temp_path = f.name

        try:
            app = FastAPI()

            @app.get("/download-named")
            def download_named():
                return FileResponse(
                    temp_path,
                    filename="custom_name.bin",
                    media_type="application/octet-stream",
                )

            client = TestClient(app)
            response = client.get("/download-named")

            assert response.status_code == 200
            assert response.content == content
            content_disp = response.headers.get("content-disposition", "")
            assert "custom_name.bin" in content_disp
        finally:
            os.unlink(temp_path)


class TestResponseClasses:
    """Tests for various Response classes."""

    def test_html_response(self):
        """Test HTMLResponse."""
        app = FastAPI()

        @app.get("/html")
        def get_html():
            return HTMLResponse("<html><body><h1>Hello</h1></body></html>")

        client = TestClient(app)
        response = client.get("/html")

        assert response.status_code == 200
        assert "text/html" in response.headers.get("content-type", "")
        assert "<h1>Hello</h1>" in response.text

    def test_plain_text_response(self):
        """Test PlainTextResponse."""
        app = FastAPI()
        text_content = f"Plain text: {secrets.token_hex(8)}"

        @app.get("/text")
        def get_text():
            return PlainTextResponse(text_content)

        client = TestClient(app)
        response = client.get("/text")

        assert response.status_code == 200
        assert "text/plain" in response.headers.get("content-type", "")
        assert response.text == text_content

    def test_json_response(self):
        """Test explicit JSONResponse."""
        app = FastAPI()

        @app.get("/json")
        def get_json():
            return JSONResponse({"key": "value", "number": 42}, status_code=201)

        client = TestClient(app)
        response = client.get("/json")

        assert response.status_code == 201
        assert "application/json" in response.headers.get("content-type", "")
        assert response.json() == {"key": "value", "number": 42}

    def test_redirect_response(self):
        """Test RedirectResponse."""
        app = FastAPI()

        @app.get("/redirect")
        def redirect():
            return RedirectResponse("/target")

        @app.get("/target")
        def target():
            return {"reached": True}

        client = TestClient(app)
        # TestClient doesn't follow redirects by default
        response = client.get("/redirect")

        assert response.status_code == 307
        assert response.headers.get("location") == "/target"

    def test_response_with_cookies(self):
        """Test Response with cookies."""
        app = FastAPI()

        @app.get("/set-cookie")
        def set_cookie():
            response = JSONResponse({"message": "Cookie set"})
            response.set_cookie(
                key="session", value=secrets.token_hex(16), httponly=True, max_age=3600
            )
            return response

        client = TestClient(app)
        response = client.get("/set-cookie")

        assert response.status_code == 200
        assert "session" in response.headers.get("set-cookie", "")

    def test_response_with_custom_status(self):
        """Test Response with custom status code."""
        app = FastAPI()

        @app.post("/created")
        def create_item():
            return Response(
                content=b'{"id": 123}', status_code=201, media_type="application/json"
            )

        client = TestClient(app)
        response = client.post("/created")

        assert response.status_code == 201
        assert response.json() == {"id": 123}


class TestMultipleRoutes:
    """Test streaming with multiple routes."""

    def test_mixed_response_types(self):
        """Test app with mixed response types."""
        app = FastAPI()

        @app.get("/json")
        def json_route():
            return {"type": "json"}

        @app.get("/html")
        def html_route():
            return HTMLResponse("<p>HTML</p>")

        @app.get("/stream")
        async def stream_route():
            async def gen():
                yield "streamed"

            return StreamingResponse(gen(), media_type="text/plain")

        @app.get("/text")
        def text_route():
            return PlainTextResponse("plain text")

        client = TestClient(app)

        # Test all routes
        resp = client.get("/json")
        assert resp.json() == {"type": "json"}

        resp = client.get("/html")
        assert "<p>HTML</p>" in resp.text

        resp = client.get("/stream")
        assert resp.text == "streamed"

        resp = client.get("/text")
        assert resp.text == "plain text"


def run_all_tests():
    """Run all test classes."""
    test_classes = [
        TestStreamingResponse,
        TestServerSentEvents,
        TestFileResponse,
        TestResponseClasses,
        TestMultipleRoutes,
    ]

    total_passed = 0
    total_failed = 0
    failures = []

    for test_class in test_classes:
        print(f"\n{test_class.__name__}:")
        instance = test_class()
        for name in dir(instance):
            if name.startswith("test_"):
                try:
                    getattr(instance, name)()
                    print(f"  ✓ {name}")
                    total_passed += 1
                except Exception as e:
                    print(f"  ✗ {name}: {e}")
                    total_failed += 1
                    failures.append((test_class.__name__, name, str(e)))

    print(f"\n{'=' * 60}")
    print(f"Results: {total_passed} passed, {total_failed} failed")

    if failures:
        print("\nFailures:")
        for cls, name, error in failures:
            print(f"  {cls}.{name}: {error}")
        return False

    return True


if __name__ == "__main__":
    import sys

    success = run_all_tests()
    sys.exit(0 if success else 1)
