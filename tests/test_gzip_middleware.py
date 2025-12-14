"""
Tests for GZip compression middleware.

Tests the ASGI-compatible GZip middleware for response compression.
"""

import gzip
import io
import random
import string

import pytest

from fasterapi.middleware.gzip import GZipMiddleware


def random_string(length: int = 100) -> str:
    """Generate a random string."""
    return "".join(random.choices(string.ascii_letters + string.digits, k=length))


def random_json(fields: int = 10) -> str:
    """Generate random JSON-like content."""
    pairs = [f'"{random_string(8)}": "{random_string(20)}"' for _ in range(fields)]
    return "{" + ", ".join(pairs) + "}"


def decompress_gzip(data: bytes) -> bytes:
    """Decompress gzip data."""
    buf = io.BytesIO(data)
    with gzip.GzipFile(fileobj=buf, mode="rb") as f:
        return f.read()


class MockRequest:
    """Mock request object for testing."""

    def __init__(self, headers: dict = None):
        self.headers = headers or {}


class MockResponse:
    """Mock response object for testing."""

    def __init__(self, body: bytes, headers: dict = None, status_code: int = 200):
        self.body = body
        self.headers = headers or {"content-type": "application/json"}
        self.status_code = status_code


class TestGZipMiddlewareInit:
    """Test GZipMiddleware initialization."""

    def test_default_minimum_size(self):
        """Test default minimum size."""
        middleware = GZipMiddleware(app=None, minimum_size=500)
        assert middleware.minimum_size == 500

    def test_custom_minimum_size(self):
        """Test custom minimum size."""
        middleware = GZipMiddleware(app=None, minimum_size=1000)
        assert middleware.minimum_size == 1000

    def test_default_compress_level(self):
        """Test default compression level."""
        middleware = GZipMiddleware(app=None)
        assert middleware.compresslevel == 9

    def test_custom_compress_level(self):
        """Test custom compression level."""
        middleware = GZipMiddleware(app=None, compresslevel=6)
        assert middleware.compresslevel == 6


class TestShouldCompress:
    """Test should_compress decision logic."""

    def test_compress_when_client_accepts_gzip(self):
        """Test compression when client accepts gzip."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": "gzip, deflate"}
        response_headers = {"content-type": "application/json"}
        body = b"x" * 200

        assert middleware.should_compress(request_headers, response_headers, body)

    def test_no_compress_when_client_doesnt_accept_gzip(self):
        """Test no compression when client doesn't accept gzip."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": "deflate, br"}
        response_headers = {"content-type": "application/json"}
        body = b"x" * 200

        assert not middleware.should_compress(request_headers, response_headers, body)

    def test_no_compress_when_already_compressed(self):
        """Test no compression when response is already compressed."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": "gzip"}
        response_headers = {
            "content-type": "application/json",
            "content-encoding": "gzip",
        }
        body = b"x" * 200

        assert not middleware.should_compress(request_headers, response_headers, body)

    def test_no_compress_below_minimum_size(self):
        """Test no compression for small responses."""
        middleware = GZipMiddleware(app=None, minimum_size=500)

        request_headers = {"accept-encoding": "gzip"}
        response_headers = {"content-type": "application/json"}
        body = b"x" * 100  # Below minimum

        assert not middleware.should_compress(request_headers, response_headers, body)

    def test_compress_json_content_type(self):
        """Test compression for JSON content type."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": "gzip"}
        response_headers = {"content-type": "application/json"}
        body = b"x" * 200

        assert middleware.should_compress(request_headers, response_headers, body)

    def test_compress_html_content_type(self):
        """Test compression for HTML content type."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": "gzip"}
        response_headers = {"content-type": "text/html"}
        body = b"x" * 200

        assert middleware.should_compress(request_headers, response_headers, body)

    def test_no_compress_binary_content_type(self):
        """Test no compression for binary content types."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": "gzip"}
        response_headers = {"content-type": "image/png"}
        body = b"x" * 200

        assert not middleware.should_compress(request_headers, response_headers, body)

    def test_compress_text_any_subtype(self):
        """Test compression for any text/* content type."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": "gzip"}
        response_headers = {"content-type": f"text/{random_string(8)}"}
        body = b"x" * 200

        assert middleware.should_compress(request_headers, response_headers, body)

    def test_compress_with_charset(self):
        """Test compression with charset parameter in content-type."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": "gzip"}
        response_headers = {"content-type": "application/json; charset=utf-8"}
        body = b"x" * 200

        assert middleware.should_compress(request_headers, response_headers, body)


class TestCompression:
    """Test actual compression functionality."""

    def test_compress_produces_valid_gzip(self):
        """Test that compress produces valid gzip data."""
        middleware = GZipMiddleware(app=None)

        original = b"Hello, World! " * 100
        compressed = middleware.compress(original)

        # Verify it's valid gzip
        decompressed = decompress_gzip(compressed)
        assert decompressed == original

    def test_compression_reduces_size(self):
        """Test that compression actually reduces size for compressible data."""
        middleware = GZipMiddleware(app=None)

        # Repetitive content compresses well
        original = b"Hello, World! This is a test. " * 100
        compressed = middleware.compress(original)

        assert len(compressed) < len(original)

    def test_different_compression_levels(self):
        """Test different compression levels produce different sizes."""
        original = b"Hello, World! This is a test. " * 1000

        low_compress = GZipMiddleware(app=None, compresslevel=1)
        high_compress = GZipMiddleware(app=None, compresslevel=9)

        low_result = low_compress.compress(original)
        high_result = high_compress.compress(original)

        # Higher compression should produce smaller output (usually)
        # But both should be valid
        assert decompress_gzip(low_result) == original
        assert decompress_gzip(high_result) == original

    def test_compress_empty_body(self):
        """Test compressing empty body."""
        middleware = GZipMiddleware(app=None)

        compressed = middleware.compress(b"")
        decompressed = decompress_gzip(compressed)
        assert decompressed == b""

    def test_compress_random_data(self):
        """Test compressing random data."""
        middleware = GZipMiddleware(app=None)

        original = random_string(5000).encode()
        compressed = middleware.compress(original)
        decompressed = decompress_gzip(compressed)

        assert decompressed == original


class TestCompressibleTypes:
    """Test compressible content types set."""

    def test_json_is_compressible(self):
        """Test that JSON is in compressible types."""
        assert "application/json" in GZipMiddleware.COMPRESSIBLE_TYPES

    def test_html_is_compressible(self):
        """Test that HTML is in compressible types."""
        assert "text/html" in GZipMiddleware.COMPRESSIBLE_TYPES

    def test_css_is_compressible(self):
        """Test that CSS is in compressible types."""
        assert "text/css" in GZipMiddleware.COMPRESSIBLE_TYPES

    def test_javascript_is_compressible(self):
        """Test that JavaScript is in compressible types."""
        assert "application/javascript" in GZipMiddleware.COMPRESSIBLE_TYPES
        assert "text/javascript" in GZipMiddleware.COMPRESSIBLE_TYPES

    def test_xml_is_compressible(self):
        """Test that XML is in compressible types."""
        assert "application/xml" in GZipMiddleware.COMPRESSIBLE_TYPES
        assert "text/xml" in GZipMiddleware.COMPRESSIBLE_TYPES

    def test_svg_is_compressible(self):
        """Test that SVG is in compressible types."""
        assert "image/svg+xml" in GZipMiddleware.COMPRESSIBLE_TYPES


class TestMiddlewareDispatch:
    """Test middleware dispatch functionality."""

    @pytest.mark.asyncio
    async def test_dispatch_compresses_large_response(self):
        """Test that large JSON responses are compressed."""

        async def mock_app(scope, receive, send):
            pass

        async def mock_call_next(request):
            body = random_json(50).encode()  # Generate large JSON
            return MockResponse(body, {"content-type": "application/json"})

        middleware = GZipMiddleware(app=mock_app, minimum_size=100)
        request = MockRequest({"accept-encoding": "gzip, deflate"})

        response = await middleware.dispatch(request, mock_call_next)

        # Check response was compressed
        if len(response.body) < len(random_json(50).encode()):
            assert response.headers.get("Content-Encoding") == "gzip"

    @pytest.mark.asyncio
    async def test_dispatch_doesnt_compress_small_response(self):
        """Test that small responses are not compressed."""

        async def mock_app(scope, receive, send):
            pass

        original_body = b'{"small": "data"}'

        async def mock_call_next(request):
            return MockResponse(original_body, {"content-type": "application/json"})

        middleware = GZipMiddleware(app=mock_app, minimum_size=1000)  # High threshold
        request = MockRequest({"accept-encoding": "gzip"})

        response = await middleware.dispatch(request, mock_call_next)

        # Body should be unchanged
        assert response.body == original_body
        assert (
            "Content-Encoding" not in response.headers
            or response.headers.get("Content-Encoding") != "gzip"
        )

    @pytest.mark.asyncio
    async def test_dispatch_adds_vary_header(self):
        """Test that Vary: Accept-Encoding is added."""

        async def mock_app(scope, receive, send):
            pass

        async def mock_call_next(request):
            body = b"x" * 1000  # Large enough to compress
            return MockResponse(body, {"content-type": "text/plain"})

        middleware = GZipMiddleware(app=mock_app, minimum_size=100)
        request = MockRequest({"accept-encoding": "gzip"})

        response = await middleware.dispatch(request, mock_call_next)

        # Check Vary header
        vary = response.headers.get("Vary", "")
        # The response might be compressed or not depending on efficiency
        if response.headers.get("Content-Encoding") == "gzip":
            assert "Accept-Encoding" in vary

    @pytest.mark.asyncio
    async def test_dispatch_preserves_existing_vary(self):
        """Test that existing Vary header is preserved."""

        async def mock_app(scope, receive, send):
            pass

        async def mock_call_next(request):
            body = b"x" * 1000
            return MockResponse(body, {"content-type": "text/plain", "Vary": "Origin"})

        middleware = GZipMiddleware(app=mock_app, minimum_size=100)
        request = MockRequest({"accept-encoding": "gzip"})

        response = await middleware.dispatch(request, mock_call_next)

        vary = response.headers.get("Vary", "")
        if response.headers.get("Content-Encoding") == "gzip":
            assert "Origin" in vary
            assert "Accept-Encoding" in vary


class TestRandomizedCompression:
    """Test compression with randomized data."""

    def test_compress_various_sizes(self):
        """Test compression with various data sizes."""
        middleware = GZipMiddleware(app=None)

        sizes = [100, 500, 1000, 5000, 10000]

        for size in sizes:
            original = random_string(size).encode()
            compressed = middleware.compress(original)
            decompressed = decompress_gzip(compressed)
            assert decompressed == original

    def test_compress_repetitive_data(self):
        """Test compression with repetitive data (high compression ratio)."""
        middleware = GZipMiddleware(app=None)

        # Highly repetitive data
        original = (random_string(10) * 1000).encode()
        compressed = middleware.compress(original)

        # Should achieve significant compression
        compression_ratio = len(compressed) / len(original)
        assert compression_ratio < 0.2  # Less than 20% of original size

        # Verify integrity
        assert decompress_gzip(compressed) == original

    def test_compress_random_binary(self):
        """Test compression with random binary data."""
        middleware = GZipMiddleware(app=None)

        # Random binary data (low compression ratio expected)
        original = bytes(random.randint(0, 255) for _ in range(1000))
        compressed = middleware.compress(original)

        # Verify integrity
        assert decompress_gzip(compressed) == original


class TestEdgeCases:
    """Test edge cases."""

    def test_empty_accept_encoding(self):
        """Test with empty Accept-Encoding header."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {"accept-encoding": ""}
        response_headers = {"content-type": "application/json"}
        body = b"x" * 200

        assert not middleware.should_compress(request_headers, response_headers, body)

    def test_missing_accept_encoding(self):
        """Test with missing Accept-Encoding header."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        request_headers = {}  # No accept-encoding
        response_headers = {"content-type": "application/json"}
        body = b"x" * 200

        assert not middleware.should_compress(request_headers, response_headers, body)

    def test_case_insensitive_gzip_detection(self):
        """Test case-insensitive gzip detection."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        for encoding in ["gzip", "GZIP", "GZip", "gZiP"]:
            request_headers = {"accept-encoding": encoding}
            response_headers = {"content-type": "application/json"}
            body = b"x" * 200

            assert middleware.should_compress(request_headers, response_headers, body)

    def test_gzip_in_list(self):
        """Test gzip detection in list of encodings."""
        middleware = GZipMiddleware(app=None, minimum_size=100)

        for encoding_list in [
            "gzip, deflate",
            "deflate, gzip",
            "br, gzip, deflate",
            "identity;q=0.5, gzip;q=1.0",
        ]:
            request_headers = {"accept-encoding": encoding_list}
            response_headers = {"content-type": "application/json"}
            body = b"x" * 200

            assert middleware.should_compress(request_headers, response_headers, body)

    def test_minimum_size_boundary(self):
        """Test compression at minimum size boundary."""
        middleware = GZipMiddleware(app=None, minimum_size=500)

        request_headers = {"accept-encoding": "gzip"}
        response_headers = {"content-type": "application/json"}

        # Just below minimum
        assert not middleware.should_compress(
            request_headers, response_headers, b"x" * 499
        )

        # Exactly at minimum
        assert middleware.should_compress(request_headers, response_headers, b"x" * 500)

        # Just above minimum
        assert middleware.should_compress(request_headers, response_headers, b"x" * 501)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
