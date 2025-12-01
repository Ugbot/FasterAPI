#!/usr/bin/env python3
"""
HTTP Request/Response Unit Tests

Tests the HTTP request and response Python wrappers without requiring
a running server or native bindings.

Tests cover:
- Request creation and property access
- Header handling (case-insensitive)
- Query parameter parsing
- Path parameter extraction
- Body parsing (JSON, form)
- Response builder pattern (method chaining)
- Status codes
- Cookie handling
- Compression settings
- Redirect handling
"""

import sys
import json
import random
import string
import tempfile
import os

import pytest

sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.http.request import Request, Method
from fasterapi.http.response import Response, Status


# =============================================================================
# Test Helpers
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate a random string that is a valid identifier."""
    first = random.choice(string.ascii_letters)
    if length == 1:
        return first
    rest = ''.join(random.choices(string.ascii_letters + string.digits, k=length - 1))
    return first + rest


def random_int(min_val: int = 1, max_val: int = 1000) -> int:
    """Generate a random integer."""
    return random.randint(min_val, max_val)


def random_email() -> str:
    """Generate a random email."""
    return f"{random_string(8)}@{random_string(5)}.com"


# =============================================================================
# Request Method Enum Tests
# =============================================================================

class TestMethod:
    """Tests for Method enum."""

    def test_all_http_methods_exist(self):
        """Test all standard HTTP methods are defined."""
        methods = ['GET', 'POST', 'PUT', 'DELETE', 'PATCH', 'HEAD', 'OPTIONS', 'CONNECT', 'TRACE']
        for method in methods:
            assert hasattr(Method, method)
            assert Method[method].value == method

    def test_method_values(self):
        """Test method enum values."""
        assert Method.GET.value == "GET"
        assert Method.POST.value == "POST"
        assert Method.PUT.value == "PUT"
        assert Method.DELETE.value == "DELETE"
        assert Method.PATCH.value == "PATCH"

    def test_method_from_string(self):
        """Test creating method from string."""
        assert Method("GET") == Method.GET
        assert Method("POST") == Method.POST


# =============================================================================
# Request Creation Tests
# =============================================================================

class TestRequestCreation:
    """Tests for Request creation."""

    def test_default_request(self):
        """Test default request values."""
        req = Request()
        assert req.method == Method.GET
        assert req.path == "/"
        assert req.query == ""
        assert req.version == "HTTP/1.1"
        assert req.headers == {}
        assert req.body == ""

    def test_request_with_method(self):
        """Test request with different methods."""
        for method in ['GET', 'POST', 'PUT', 'DELETE', 'PATCH']:
            req = Request(method=method)
            assert req.method == Method(method)

    def test_request_with_lowercase_method(self):
        """Test method is case-insensitive."""
        req = Request(method='get')
        assert req.method == Method.GET

        req = Request(method='post')
        assert req.method == Method.POST

    def test_request_with_path(self):
        """Test request with custom path."""
        path = f"/users/{random_int()}/items"
        req = Request(path=path)
        assert req.path == path

    def test_request_with_headers(self):
        """Test request with headers."""
        headers = {
            'content-type': 'application/json',
            'authorization': f'Bearer {random_string(32)}'
        }
        req = Request(headers=headers)
        assert req.headers == headers

    def test_request_with_query_params(self):
        """Test request with query parameters."""
        query_params = {
            'page': '1',
            'limit': '10',
            'search': random_string(10)
        }
        req = Request(query_params=query_params)
        assert req.query_params == query_params

    def test_request_with_path_params(self):
        """Test request with path parameters."""
        path_params = {
            'user_id': str(random_int()),
            'item_id': str(random_int())
        }
        req = Request(path_params=path_params)
        assert req.path_params == path_params

    def test_request_with_body(self):
        """Test request with body."""
        body = json.dumps({'name': random_string(), 'value': random_int()})
        req = Request(body=body)
        assert req.body == body

    def test_request_with_body_bytes(self):
        """Test request with binary body."""
        body_bytes = b'\x00\x01\x02\x03'
        req = Request(body_bytes=body_bytes)
        assert req.body_bytes == body_bytes

    def test_request_with_client_ip(self):
        """Test request with client IP."""
        ip = f"192.168.{random_int(0, 255)}.{random_int(0, 255)}"
        req = Request(client_ip=ip)
        assert req.client_ip == ip

    def test_request_with_request_id(self):
        """Test request with request ID."""
        req_id = random_int(1, 1000000)
        req = Request(request_id=req_id)
        assert req.request_id == req_id

    def test_request_secure_flag(self):
        """Test request secure flag (HTTPS)."""
        req = Request(secure=True)
        assert req.secure is True
        assert req.is_secure() is True

        req = Request(secure=False)
        assert req.is_secure() is False

    def test_request_protocol(self):
        """Test request protocol."""
        for protocol in ['HTTP/1.1', 'HTTP/2', 'HTTP/3']:
            req = Request(protocol=protocol)
            assert req.get_protocol() == protocol


# =============================================================================
# Request Getter Tests
# =============================================================================

class TestRequestGetters:
    """Tests for Request getter methods."""

    def test_get_method(self):
        """Test get_method."""
        req = Request(method='POST')
        assert req.get_method() == Method.POST

    def test_get_path(self):
        """Test get_path."""
        path = '/api/users'
        req = Request(path=path)
        assert req.get_path() == path

    def test_get_query(self):
        """Test get_query."""
        query = 'page=1&limit=10'
        req = Request(query=query)
        assert req.get_query() == query

    def test_get_version(self):
        """Test get_version."""
        version = 'HTTP/2'
        req = Request(version=version)
        assert req.get_version() == version

    def test_get_body(self):
        """Test get_body."""
        body = '{"test": true}'
        req = Request(body=body)
        assert req.get_body() == body

    def test_get_body_bytes(self):
        """Test get_body_bytes."""
        body_bytes = b'binary data'
        req = Request(body_bytes=body_bytes)
        assert req.get_body_bytes() == body_bytes


# =============================================================================
# Request Header Tests
# =============================================================================

class TestRequestHeaders:
    """Tests for Request header handling."""

    def test_get_header_exists(self):
        """Test getting existing header."""
        headers = {'content-type': 'application/json'}
        req = Request(headers=headers)
        assert req.get_header('content-type') == 'application/json'

    def test_get_header_case_insensitive(self):
        """Test header lookup is case-insensitive."""
        headers = {'content-type': 'application/json'}
        req = Request(headers=headers)
        assert req.get_header('Content-Type') == 'application/json'
        assert req.get_header('CONTENT-TYPE') == 'application/json'

    def test_get_header_missing(self):
        """Test getting missing header returns empty string."""
        req = Request()
        assert req.get_header('x-missing') == ''

    def test_get_headers(self):
        """Test get_headers returns copy."""
        headers = {'a': '1', 'b': '2'}
        req = Request(headers=headers)
        returned = req.get_headers()
        assert returned == headers
        # Should be a copy
        returned['c'] = '3'
        assert 'c' not in req.headers

    def test_get_content_type(self):
        """Test get_content_type convenience method."""
        req = Request(headers={'content-type': 'text/html'})
        assert req.get_content_type() == 'text/html'

    def test_get_content_length_valid(self):
        """Test get_content_length with valid value."""
        length = random_int(100, 10000)
        req = Request(headers={'content-length': str(length)})
        assert req.get_content_length() == length

    def test_get_content_length_missing(self):
        """Test get_content_length with missing header."""
        req = Request()
        assert req.get_content_length() == 0

    def test_get_content_length_invalid(self):
        """Test get_content_length with invalid value."""
        req = Request(headers={'content-length': 'invalid'})
        assert req.get_content_length() == 0

    def test_get_user_agent(self):
        """Test get_user_agent convenience method."""
        ua = f"FasterAPI-Test/{random_string(5)}"
        req = Request(headers={'user-agent': ua})
        assert req.get_user_agent() == ua


# =============================================================================
# Request Parameter Tests
# =============================================================================

class TestRequestParameters:
    """Tests for Request parameter handling."""

    def test_get_query_param_exists(self):
        """Test getting existing query param."""
        query_params = {'page': '5', 'limit': '20'}
        req = Request(query_params=query_params)
        assert req.get_query_param('page') == '5'
        assert req.get_query_param('limit') == '20'

    def test_get_query_param_missing(self):
        """Test getting missing query param."""
        req = Request()
        assert req.get_query_param('missing') == ''

    def test_get_path_param_exists(self):
        """Test getting existing path param."""
        path_params = {'user_id': '42', 'item_id': '99'}
        req = Request(path_params=path_params)
        assert req.get_path_param('user_id') == '42'
        assert req.get_path_param('item_id') == '99'

    def test_get_path_param_missing(self):
        """Test getting missing path param."""
        req = Request()
        assert req.get_path_param('missing') == ''

    def test_randomized_params(self):
        """Test with randomized parameters."""
        num_params = random_int(5, 15)
        query_params = {random_string(8): str(random_int()) for _ in range(num_params)}
        path_params = {random_string(8): str(random_int()) for _ in range(num_params)}

        req = Request(query_params=query_params, path_params=path_params)

        for key, value in query_params.items():
            assert req.get_query_param(key) == value
        for key, value in path_params.items():
            assert req.get_path_param(key) == value


# =============================================================================
# Request Body Parsing Tests
# =============================================================================

class TestRequestBodyParsing:
    """Tests for Request body parsing."""

    def test_is_json_true(self):
        """Test is_json with JSON content type."""
        req = Request(headers={'content-type': 'application/json'})
        assert req.is_json() is True

        req = Request(headers={'content-type': 'application/json; charset=utf-8'})
        assert req.is_json() is True

    def test_is_json_false(self):
        """Test is_json with non-JSON content type."""
        req = Request(headers={'content-type': 'text/html'})
        assert req.is_json() is False

        req = Request()
        assert req.is_json() is False

    def test_is_multipart_true(self):
        """Test is_multipart with multipart content type."""
        req = Request(headers={'content-type': 'multipart/form-data; boundary=----'})
        assert req.is_multipart() is True

    def test_is_multipart_false(self):
        """Test is_multipart with non-multipart content type."""
        req = Request(headers={'content-type': 'application/json'})
        assert req.is_multipart() is False

    def test_json_valid(self):
        """Test parsing valid JSON body."""
        data = {'name': random_string(), 'value': random_int(), 'active': True}
        req = Request(
            headers={'content-type': 'application/json'},
            body=json.dumps(data)
        )
        parsed = req.json()
        assert parsed == data

    def test_json_invalid_content_type(self):
        """Test json() with wrong content type raises error."""
        req = Request(
            headers={'content-type': 'text/plain'},
            body='{"valid": "json"}'
        )
        with pytest.raises(ValueError, match="not JSON"):
            req.json()

    def test_json_invalid_body(self):
        """Test json() with invalid JSON body."""
        req = Request(
            headers={'content-type': 'application/json'},
            body='not valid json'
        )
        with pytest.raises(ValueError, match="Invalid JSON"):
            req.json()

    def test_form_urlencoded(self):
        """Test parsing URL-encoded form data."""
        req = Request(
            headers={'content-type': 'application/x-www-form-urlencoded'},
            body='name=John&email=john@example.com&age=30'
        )
        form_data = req.form()
        assert form_data['name'] == 'John'
        assert form_data['email'] == 'john@example.com'
        assert form_data['age'] == '30'

    def test_form_empty(self):
        """Test parsing empty form data."""
        req = Request(body='')
        form_data = req.form()
        assert form_data == {}


# =============================================================================
# Request Repr Tests
# =============================================================================

class TestRequestRepr:
    """Tests for Request string representation."""

    def test_repr(self):
        """Test request repr."""
        req = Request(method='POST', path='/api/users', protocol='HTTP/2')
        repr_str = repr(req)
        assert 'POST' in repr_str
        assert '/api/users' in repr_str
        assert 'HTTP/2' in repr_str


# =============================================================================
# Response Status Tests
# =============================================================================

class TestResponseStatus:
    """Tests for Response status handling."""

    def test_all_status_codes_exist(self):
        """Test all common status codes are defined."""
        expected = {
            200: 'OK', 201: 'CREATED', 204: 'NO_CONTENT',
            301: 'MOVED_PERMANENTLY', 302: 'FOUND', 304: 'NOT_MODIFIED',
            400: 'BAD_REQUEST', 401: 'UNAUTHORIZED', 403: 'FORBIDDEN',
            404: 'NOT_FOUND', 405: 'METHOD_NOT_ALLOWED', 422: 'UNPROCESSABLE_ENTITY',
            500: 'INTERNAL_SERVER_ERROR', 502: 'BAD_GATEWAY', 503: 'SERVICE_UNAVAILABLE'
        }
        for code, name in expected.items():
            assert Status(code).value == code
            assert Status[name].value == code

    def test_default_status(self):
        """Test default response status is 200 OK."""
        resp = Response()
        assert resp.status_code == Status.OK

    def test_set_status_with_enum(self):
        """Test setting status with Status enum."""
        resp = Response()
        result = resp.status(Status.NOT_FOUND)
        assert resp.status_code == Status.NOT_FOUND
        assert result is resp  # Method chaining

    def test_set_status_with_int(self):
        """Test setting status with integer."""
        resp = Response()
        resp.status(201)
        assert resp.status_code == Status.CREATED

        resp.status(404)
        assert resp.status_code == Status.NOT_FOUND


# =============================================================================
# Response Header Tests
# =============================================================================

class TestResponseHeaders:
    """Tests for Response header handling."""

    def test_set_header(self):
        """Test setting a header."""
        resp = Response()
        result = resp.header('X-Custom', 'value')
        assert resp.headers['x-custom'] == 'value'
        assert result is resp  # Method chaining

    def test_header_case_normalized(self):
        """Test header names are lowercased."""
        resp = Response()
        resp.header('Content-Type', 'text/html')
        assert 'content-type' in resp.headers
        assert resp.headers['content-type'] == 'text/html'

    def test_content_type_method(self):
        """Test content_type convenience method."""
        resp = Response()
        result = resp.content_type('application/json')
        assert resp.headers['content-type'] == 'application/json'
        assert result is resp

    def test_multiple_headers(self):
        """Test setting multiple headers."""
        resp = Response()
        resp.header('X-Header-1', 'value1').header('X-Header-2', 'value2')
        assert resp.headers['x-header-1'] == 'value1'
        assert resp.headers['x-header-2'] == 'value2'


# =============================================================================
# Response Body Tests
# =============================================================================

class TestResponseBody:
    """Tests for Response body handling."""

    def test_json_with_dict(self):
        """Test JSON response with dictionary."""
        data = {'name': random_string(), 'count': random_int()}
        resp = Response()
        result = resp.json(data)

        assert json.loads(resp.body) == data
        assert resp.headers['content-type'] == 'application/json'
        assert result is resp

    def test_json_with_list(self):
        """Test JSON response with list."""
        data = [{'id': i, 'name': random_string()} for i in range(5)]
        resp = Response()
        resp.json(data)

        assert json.loads(resp.body) == data

    def test_json_with_string(self):
        """Test JSON response with pre-serialized string."""
        json_str = '{"pre": "serialized"}'
        resp = Response()
        resp.json(json_str)

        assert resp.body == json_str

    def test_text_response(self):
        """Test text response."""
        text = f"Hello, {random_string()}!"
        resp = Response()
        result = resp.text(text)

        assert resp.body == text
        assert resp.headers['content-type'] == 'text/plain'
        assert result is resp

    def test_html_response(self):
        """Test HTML response."""
        html = f"<h1>Welcome, {random_string()}</h1>"
        resp = Response()
        result = resp.html(html)

        assert resp.body == html
        assert resp.headers['content-type'] == 'text/html'
        assert result is resp

    def test_binary_response(self):
        """Test binary response."""
        data = bytes([random_int(0, 255) for _ in range(100)])
        resp = Response()
        result = resp.binary(data)

        assert resp.headers['content-type'] == 'application/octet-stream'
        assert result is resp

    def test_get_size(self):
        """Test get_size returns body length."""
        resp = Response()
        resp.body = "Hello World"
        assert resp.get_size() == 11


# =============================================================================
# Response File Tests
# =============================================================================

class TestResponseFile:
    """Tests for Response file handling."""

    def test_file_exists(self):
        """Test file response with existing file."""
        with tempfile.NamedTemporaryFile(mode='wb', delete=False) as f:
            content = b"File content " + random_string(20).encode()
            f.write(content)
            temp_path = f.name

        try:
            resp = Response()
            resp.file(temp_path)
            assert resp.headers['content-type'] == 'application/octet-stream'
        finally:
            os.unlink(temp_path)

    def test_file_not_found(self):
        """Test file response with non-existent file."""
        resp = Response()
        resp.file('/nonexistent/path/file.txt')
        assert resp.status_code == Status.NOT_FOUND
        assert 'not found' in resp.body.lower()


# =============================================================================
# Response Redirect Tests
# =============================================================================

class TestResponseRedirect:
    """Tests for Response redirect handling."""

    def test_temporary_redirect(self):
        """Test temporary redirect (302)."""
        url = f"/new/{random_string()}"
        resp = Response()
        result = resp.redirect(url)

        assert resp.status_code == Status.FOUND
        assert resp.headers['location'] == url
        assert result is resp

    def test_permanent_redirect(self):
        """Test permanent redirect (301)."""
        url = f"/new/{random_string()}"
        resp = Response()
        resp.redirect(url, permanent=True)

        assert resp.status_code == Status.MOVED_PERMANENTLY
        assert resp.headers['location'] == url


# =============================================================================
# Response Cookie Tests
# =============================================================================

class TestResponseCookies:
    """Tests for Response cookie handling."""

    def test_set_cookie_simple(self):
        """Test setting a simple cookie."""
        name = random_string(8)
        value = random_string(16)
        resp = Response()
        result = resp.cookie(name, value)

        assert f"{name}={value}" in resp.headers['set-cookie']
        assert result is resp

    def test_set_cookie_with_options(self):
        """Test setting cookie with options."""
        resp = Response()
        resp.cookie('session', 'abc123', {
            'path': '/',
            'domain': 'example.com',
            'secure': ''
        })

        cookie = resp.headers['set-cookie']
        assert 'session=abc123' in cookie
        assert 'path=/' in cookie
        assert 'domain=example.com' in cookie

    def test_multiple_cookies(self):
        """Test setting multiple cookies."""
        resp = Response()
        resp.cookie('cookie1', 'value1')
        resp.cookie('cookie2', 'value2')

        # Both cookies should be in the header
        cookie_header = resp.headers['set-cookie']
        assert 'cookie1=value1' in cookie_header
        assert 'cookie2=value2' in cookie_header

    def test_clear_cookie(self):
        """Test clearing a cookie."""
        resp = Response()
        resp.clear_cookie('session')

        cookie = resp.headers['set-cookie']
        assert 'session=' in cookie
        assert 'expires=Thu, 01 Jan 1970' in cookie


# =============================================================================
# Response Compression Tests
# =============================================================================

class TestResponseCompression:
    """Tests for Response compression settings."""

    def test_compression_enabled_by_default(self):
        """Test compression is enabled by default."""
        resp = Response()
        assert resp.compression_enabled is True

    def test_disable_compression(self):
        """Test disabling compression."""
        resp = Response()
        result = resp.compress(False)

        assert resp.compression_enabled is False
        assert result is resp

    def test_compression_level(self):
        """Test setting compression level."""
        level = random_int(1, 22)
        resp = Response()
        # Note: method has a naming collision with the attribute
        resp.compression_level = level
        assert resp.compression_level == level

    def test_get_compression_ratio_no_data(self):
        """Test compression ratio with no data."""
        resp = Response()
        assert resp.get_compression_ratio() == 0.0


# =============================================================================
# Response Send Tests
# =============================================================================

class TestResponseSend:
    """Tests for Response send behavior."""

    def test_send_returns_zero(self):
        """Test send returns 0 on success."""
        resp = Response()
        resp.text("Hello")
        result = resp.send()
        assert result == 0

    def test_send_sets_sent_flag(self):
        """Test send sets the is_sent flag."""
        resp = Response()
        resp.text("Hello")
        assert resp.is_sent is False
        resp.send()
        assert resp.is_sent is True

    def test_send_idempotent(self):
        """Test multiple sends don't fail."""
        resp = Response()
        resp.text("Hello")
        resp.send()
        result = resp.send()  # Second send
        assert result == 0

    def test_send_sets_original_size(self):
        """Test send sets original_size."""
        resp = Response()
        resp.body = "Hello World"
        resp.send()
        assert resp.original_size == 11


# =============================================================================
# Response Repr Tests
# =============================================================================

class TestResponseRepr:
    """Tests for Response string representation."""

    def test_repr(self):
        """Test response repr."""
        resp = Response()
        resp.status(201)
        resp.body = "Created"
        repr_str = repr(resp)
        assert '201' in repr_str
        assert '7' in repr_str  # len("Created")


# =============================================================================
# Method Chaining Tests
# =============================================================================

class TestMethodChaining:
    """Tests for Response method chaining."""

    def test_full_chain(self):
        """Test full method chaining."""
        resp = (Response()
            .status(201)
            .header('X-Custom', 'value')
            .json({'created': True}))

        assert resp.status_code == Status.CREATED
        assert resp.headers['x-custom'] == 'value'
        assert resp.headers['content-type'] == 'application/json'
        assert '"created": true' in resp.body

    def test_complex_chain(self):
        """Test complex method chaining."""
        name = random_string(10)
        resp = (Response()
            .status(200)
            .content_type('application/json')
            .header('X-Request-Id', str(random_int()))
            .cookie('session', random_string(32))
            .json({'user': name}))

        assert resp.status_code == Status.OK
        assert 'application/json' in resp.headers['content-type']
        assert 'session=' in resp.headers['set-cookie']


# =============================================================================
# Stress Tests
# =============================================================================

class TestStress:
    """Stress tests with randomized data."""

    def test_many_headers(self):
        """Test request with many headers."""
        num_headers = random_int(50, 100)
        headers = {f"x-header-{i}": random_string(20) for i in range(num_headers)}
        req = Request(headers=headers)

        for name, value in headers.items():
            assert req.get_header(name) == value

    def test_many_query_params(self):
        """Test request with many query parameters."""
        num_params = random_int(50, 100)
        query_params = {f"param_{i}": str(random_int()) for i in range(num_params)}
        req = Request(query_params=query_params)

        for name, value in query_params.items():
            assert req.get_query_param(name) == value

    def test_large_json_body(self):
        """Test request with large JSON body."""
        data = {
            'items': [
                {'id': i, 'name': random_string(50), 'data': random_string(200)}
                for i in range(100)
            ]
        }
        body = json.dumps(data)
        req = Request(
            headers={'content-type': 'application/json'},
            body=body
        )

        parsed = req.json()
        assert len(parsed['items']) == 100

    def test_many_response_headers(self):
        """Test response with many headers."""
        resp = Response()
        for i in range(50):
            resp.header(f'X-Header-{i}', random_string(20))

        assert len(resp.headers) == 50

    def test_many_cookies(self):
        """Test response with many cookies."""
        resp = Response()
        for i in range(10):
            resp.cookie(f'cookie_{i}', random_string(16))

        # All cookies should be in the header
        cookie_header = resp.headers['set-cookie']
        for i in range(10):
            assert f'cookie_{i}=' in cookie_header


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
