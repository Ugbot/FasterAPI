#!/usr/bin/env python3.13
"""
Comprehensive tests for ZmqWorker (ZeroMQ-based IPC worker).

Tests:
- Message serialization/deserialization (parse_request, serialize_response)
- Handler caching and lookup (get_handler)
- Request processing (sync and async handlers)
- Error handling and edge cases
- Worker statistics tracking
- Uses randomized inputs per project guidelines
"""

import pytest
import asyncio
import struct
import json
import random
import string
import sys
import types
import os
from unittest.mock import Mock, AsyncMock, patch, MagicMock

# Import the module under test
from fasterapi.core.zmq_worker import (
    ZmqWorker,
    MessageType,
    worker_main,
    start_worker,
)


# =============================================================================
# Test Fixtures
# =============================================================================

@pytest.fixture
def worker():
    """Create a ZmqWorker instance for testing."""
    ipc_prefix = f"test_{random.randint(10000, 99999)}"
    worker_id = random.randint(1, 100)
    return ZmqWorker(ipc_prefix, worker_id)


@pytest.fixture
def test_handlers_module():
    """Create a test module with various handler functions."""
    module_name = f'test_handlers_{random.randint(10000, 99999)}'
    module = types.ModuleType(module_name)
    sys.modules[module_name] = module

    # Define handlers in the module
    exec('''
import asyncio

def sync_handler():
    """Simple synchronous handler."""
    return {"status": "ok", "type": "sync"}

async def async_handler():
    """Simple asynchronous handler."""
    return {"status": "ok", "type": "async"}

def handler_with_params(value: int, name: str):
    """Handler with parameters."""
    return {"value": value, "name": name}

def handler_with_defaults(value: int = 42, name: str = "default"):
    """Handler with default parameters."""
    return {"value": value, "name": name}

async def async_handler_with_params(count: int, prefix: str):
    """Async handler with parameters."""
    await asyncio.sleep(0.001)  # Simulate async work
    return {"count": count, "prefix": prefix}

def handler_that_raises():
    """Handler that raises an exception."""
    raise ValueError("Test exception")

def handler_returns_complex():
    """Handler returning complex nested data."""
    return {
        "list": [1, 2, 3],
        "nested": {"a": 1, "b": 2},
        "string": "hello",
        "number": 42.5,
        "bool": True,
        "null": None
    }

def handler_with_unicode(text: str):
    """Handler with unicode parameters."""
    return {"text": text, "length": len(text)}
''', module.__dict__)

    yield module_name, module

    # Cleanup
    del sys.modules[module_name]


# =============================================================================
# Message Serialization Tests
# =============================================================================

class TestMessageSerialization:
    """Test request/response serialization."""

    def test_parse_request_valid(self, worker):
        """Test parsing a valid request message."""
        module_name = "test_module"
        function_name = "test_function"
        kwargs = {"key": "value", "number": 42}
        kwargs_json = json.dumps(kwargs)

        # Build request message
        message = self._build_request(
            msg_type=MessageType.REQUEST,
            request_id=12345,
            module_name=module_name,
            function_name=function_name,
            kwargs_json=kwargs_json
        )

        result = worker.parse_request(message)

        assert result is not None
        req_id, mod_name, func_name, parsed_kwargs = result
        assert req_id == 12345
        assert mod_name == module_name
        assert func_name == function_name
        assert parsed_kwargs == kwargs

    def test_parse_request_shutdown(self, worker):
        """Test parsing a shutdown message."""
        message = self._build_request(
            msg_type=MessageType.SHUTDOWN,
            request_id=0,
            module_name="",
            function_name="",
            kwargs_json=""
        )

        result = worker.parse_request(message)
        assert result is None  # Shutdown returns None

    def test_parse_request_too_short(self, worker):
        """Test parsing message that's too short."""
        short_message = b'\x01' * 10  # Less than 21 bytes header
        result = worker.parse_request(short_message)
        assert result is None

    def test_parse_request_empty_kwargs(self, worker):
        """Test parsing request with empty kwargs."""
        message = self._build_request(
            msg_type=MessageType.REQUEST,
            request_id=999,
            module_name="mod",
            function_name="func",
            kwargs_json=""
        )

        result = worker.parse_request(message)
        assert result is not None
        _, _, _, kwargs = result
        assert kwargs == {}

    def test_parse_request_invalid_json_kwargs(self, worker):
        """Test parsing request with invalid JSON in kwargs."""
        message = self._build_request(
            msg_type=MessageType.REQUEST,
            request_id=999,
            module_name="mod",
            function_name="func",
            kwargs_json="not valid json {"
        )

        result = worker.parse_request(message)
        assert result is not None
        _, _, _, kwargs = result
        assert kwargs == {}  # Falls back to empty dict

    def test_parse_request_randomized(self, worker):
        """Test parsing with randomized data."""
        for _ in range(20):
            request_id = random.randint(1, 2**31)
            module_name = ''.join(random.choices(string.ascii_lowercase, k=random.randint(5, 20)))
            function_name = ''.join(random.choices(string.ascii_lowercase, k=random.randint(5, 15)))
            kwargs = {
                ''.join(random.choices(string.ascii_lowercase, k=5)): random.randint(-1000, 1000)
                for _ in range(random.randint(0, 5))
            }
            kwargs_json = json.dumps(kwargs)

            message = self._build_request(
                msg_type=MessageType.REQUEST,
                request_id=request_id,
                module_name=module_name,
                function_name=function_name,
                kwargs_json=kwargs_json
            )

            result = worker.parse_request(message)
            assert result is not None
            req_id, mod, func, parsed_kwargs = result
            assert req_id == request_id
            assert mod == module_name
            assert func == function_name
            assert parsed_kwargs == kwargs

    def test_serialize_response_success(self, worker):
        """Test serializing a successful response."""
        request_id = 12345
        status_code = 200
        body = {"result": "success", "data": [1, 2, 3]}
        body_json = json.dumps(body)

        response = worker.serialize_response(
            request_id=request_id,
            status_code=status_code,
            success=True,
            body_json=body_json
        )

        # Parse the response to verify
        parsed = self._parse_response(response)
        assert parsed['type'] == MessageType.RESPONSE
        assert parsed['request_id'] == request_id
        assert parsed['status_code'] == status_code
        assert parsed['success'] == True
        assert parsed['body'] == body_json
        assert parsed['error'] == ""

    def test_serialize_response_error(self, worker):
        """Test serializing an error response."""
        request_id = 54321
        status_code = 500
        error_msg = "Something went wrong"
        body_json = json.dumps({"error": error_msg})

        response = worker.serialize_response(
            request_id=request_id,
            status_code=status_code,
            success=False,
            body_json=body_json,
            error_message=error_msg
        )

        parsed = self._parse_response(response)
        assert parsed['type'] == MessageType.RESPONSE
        assert parsed['request_id'] == request_id
        assert parsed['status_code'] == status_code
        assert parsed['success'] == False
        assert error_msg in parsed['error']

    def test_serialize_response_unicode(self, worker):
        """Test serializing response with unicode content."""
        body = {"message": "Hello 世界 🌍 مرحبا"}
        body_json = json.dumps(body, ensure_ascii=False)

        response = worker.serialize_response(
            request_id=1,
            status_code=200,
            success=True,
            body_json=body_json
        )

        parsed = self._parse_response(response)
        assert parsed['body'] == body_json

    def test_serialize_response_large_body(self, worker):
        """Test serializing response with large body."""
        # Generate a large random body
        large_data = ''.join(random.choices(string.ascii_letters, k=100000))
        body_json = json.dumps({"data": large_data})

        response = worker.serialize_response(
            request_id=1,
            status_code=200,
            success=True,
            body_json=body_json
        )

        parsed = self._parse_response(response)
        assert parsed['body'] == body_json

    def _build_request(self, msg_type, request_id, module_name, function_name, kwargs_json):
        """Helper to build a request message."""
        module_bytes = module_name.encode('utf-8')
        function_bytes = function_name.encode('utf-8')
        kwargs_bytes = kwargs_json.encode('utf-8')

        # Header: type(1) + request_id(4) + module_len(4) + func_len(4) + kwargs_len(4) = 17 bytes
        # Note: The worker's parse_request uses format <BIIIII which is 21 bytes
        # But it only parses 5 fields. We need to match what the worker expects.
        header = struct.pack(
            '<BIIIII',  # B + 5*I = 21 bytes
            msg_type,
            request_id,
            len(module_bytes),
            len(function_bytes),
            len(kwargs_bytes),
            0  # padding to match the 6-item format
        )

        return header + module_bytes + function_bytes + kwargs_bytes

    def _parse_response(self, data):
        """Helper to parse a response message."""
        # Response header: type(1) + request_id(4) + total_length(4) + status_code(2) + body_len(4) + error_len(4) + success(1) = 20 bytes
        header_format = '<BIIHIIB'
        header_size = struct.calcsize(header_format)  # Should be 20

        header = struct.unpack_from(header_format, data, 0)

        msg_type = header[0]
        request_id = header[1]
        total_length = header[2]
        status_code = header[3]
        body_len = header[4]
        error_len = header[5]
        success = header[6]

        offset = header_size
        body = data[offset:offset + body_len].decode('utf-8')
        offset += body_len
        error = data[offset:offset + error_len].decode('utf-8')

        return {
            'type': msg_type,
            'request_id': request_id,
            'total_length': total_length,
            'status_code': status_code,
            'success': bool(success),
            'body': body,
            'error': error
        }


# =============================================================================
# Handler Caching Tests
# =============================================================================

class TestHandlerCaching:
    """Test handler lookup and caching."""

    def test_get_handler_success(self, worker, test_handlers_module):
        """Test getting a valid handler."""
        module_name, module = test_handlers_module

        handler = worker.get_handler(module_name, 'sync_handler')

        assert handler is not None
        assert callable(handler)
        assert handler() == {"status": "ok", "type": "sync"}

    def test_get_handler_caching(self, worker, test_handlers_module):
        """Test that handlers are cached."""
        module_name, module = test_handlers_module

        # First call
        handler1 = worker.get_handler(module_name, 'sync_handler')
        initial_cache_count = worker.stats['handlers_cached']

        # Second call - should use cache
        handler2 = worker.get_handler(module_name, 'sync_handler')

        assert handler1 is handler2
        assert worker.stats['handlers_cached'] == initial_cache_count  # No increment

    def test_get_handler_module_caching(self, worker, test_handlers_module):
        """Test that modules are cached."""
        module_name, module = test_handlers_module

        worker.get_handler(module_name, 'sync_handler')

        assert module_name in worker.module_cache
        assert worker.module_cache[module_name] is module

    def test_get_handler_invalid_module(self, worker):
        """Test getting handler from non-existent module."""
        with pytest.raises(ImportError):
            worker.get_handler('nonexistent_module_xyz', 'some_function')

    def test_get_handler_invalid_function(self, worker, test_handlers_module):
        """Test getting non-existent function from valid module."""
        module_name, _ = test_handlers_module

        with pytest.raises(AttributeError) as exc_info:
            worker.get_handler(module_name, 'nonexistent_function')

        assert 'nonexistent_function' in str(exc_info.value)

    def test_get_handler_multiple_functions(self, worker, test_handlers_module):
        """Test getting multiple different functions."""
        module_name, module = test_handlers_module

        functions = ['sync_handler', 'async_handler', 'handler_with_params']
        handlers = []

        for func_name in functions:
            handler = worker.get_handler(module_name, func_name)
            handlers.append(handler)

        # All should be different functions
        assert len(set(id(h) for h in handlers)) == len(functions)
        assert worker.stats['handlers_cached'] == len(functions)


# =============================================================================
# Request Processing Tests
# =============================================================================

class TestRequestProcessing:
    """Test request processing with sync and async handlers."""

    @pytest.mark.asyncio
    async def test_process_sync_handler(self, worker, test_handlers_module):
        """Test processing request with sync handler."""
        module_name, _ = test_handlers_module

        # Mock the response socket
        worker.response_socket = AsyncMock()

        await worker.process_request(
            request_id=1,
            module_name=module_name,
            function_name='sync_handler',
            kwargs={}
        )

        # Verify response was sent
        worker.response_socket.send.assert_called_once()
        response_data = worker.response_socket.send.call_args[0][0]

        # Parse and verify response
        parsed = self._parse_response(response_data)
        assert parsed['request_id'] == 1
        assert parsed['status_code'] == 200
        assert parsed['success'] == True

        body = json.loads(parsed['body'])
        assert body['status'] == 'ok'
        assert body['type'] == 'sync'

        assert worker.stats['requests_processed'] == 1
        assert worker.stats['requests_failed'] == 0

    @pytest.mark.asyncio
    async def test_process_async_handler(self, worker, test_handlers_module):
        """Test processing request with async handler."""
        module_name, _ = test_handlers_module

        worker.response_socket = AsyncMock()

        await worker.process_request(
            request_id=2,
            module_name=module_name,
            function_name='async_handler',
            kwargs={}
        )

        worker.response_socket.send.assert_called_once()
        response_data = worker.response_socket.send.call_args[0][0]

        parsed = self._parse_response(response_data)
        assert parsed['status_code'] == 200

        body = json.loads(parsed['body'])
        assert body['status'] == 'ok'
        assert body['type'] == 'async'

    @pytest.mark.asyncio
    async def test_process_handler_with_params(self, worker, test_handlers_module):
        """Test processing request with parameters."""
        module_name, _ = test_handlers_module

        worker.response_socket = AsyncMock()

        # Randomized parameters
        value = random.randint(-1000, 1000)
        name = ''.join(random.choices(string.ascii_letters, k=10))

        await worker.process_request(
            request_id=3,
            module_name=module_name,
            function_name='handler_with_params',
            kwargs={'value': value, 'name': name}
        )

        response_data = worker.response_socket.send.call_args[0][0]
        parsed = self._parse_response(response_data)
        body = json.loads(parsed['body'])

        assert body['value'] == value
        assert body['name'] == name

    @pytest.mark.asyncio
    async def test_process_handler_with_defaults(self, worker, test_handlers_module):
        """Test processing request with default parameters."""
        module_name, _ = test_handlers_module

        worker.response_socket = AsyncMock()

        # Call with no kwargs - should use defaults
        await worker.process_request(
            request_id=4,
            module_name=module_name,
            function_name='handler_with_defaults',
            kwargs={}
        )

        response_data = worker.response_socket.send.call_args[0][0]
        parsed = self._parse_response(response_data)
        body = json.loads(parsed['body'])

        assert body['value'] == 42
        assert body['name'] == 'default'

    @pytest.mark.asyncio
    async def test_process_handler_exception(self, worker, test_handlers_module):
        """Test processing request where handler raises exception."""
        module_name, _ = test_handlers_module

        worker.response_socket = AsyncMock()

        await worker.process_request(
            request_id=5,
            module_name=module_name,
            function_name='handler_that_raises',
            kwargs={}
        )

        response_data = worker.response_socket.send.call_args[0][0]
        parsed = self._parse_response(response_data)

        assert parsed['status_code'] == 500
        assert parsed['success'] == False
        assert 'ValueError' in parsed['error']
        assert 'Test exception' in parsed['error']

        assert worker.stats['requests_failed'] == 1

    @pytest.mark.asyncio
    async def test_process_invalid_module(self, worker):
        """Test processing request with invalid module."""
        worker.response_socket = AsyncMock()

        await worker.process_request(
            request_id=6,
            module_name='nonexistent_module_xyz',
            function_name='some_function',
            kwargs={}
        )

        response_data = worker.response_socket.send.call_args[0][0]
        parsed = self._parse_response(response_data)

        assert parsed['status_code'] == 500
        assert parsed['success'] == False
        assert worker.stats['requests_failed'] == 1

    @pytest.mark.asyncio
    async def test_process_complex_return_value(self, worker, test_handlers_module):
        """Test processing request with complex return value."""
        module_name, _ = test_handlers_module

        worker.response_socket = AsyncMock()

        await worker.process_request(
            request_id=7,
            module_name=module_name,
            function_name='handler_returns_complex',
            kwargs={}
        )

        response_data = worker.response_socket.send.call_args[0][0]
        parsed = self._parse_response(response_data)
        body = json.loads(parsed['body'])

        assert body['list'] == [1, 2, 3]
        assert body['nested'] == {"a": 1, "b": 2}
        assert body['string'] == "hello"
        assert body['number'] == 42.5
        assert body['bool'] == True
        assert body['null'] is None

    @pytest.mark.asyncio
    async def test_process_unicode_params(self, worker, test_handlers_module):
        """Test processing request with unicode parameters."""
        module_name, _ = test_handlers_module

        worker.response_socket = AsyncMock()

        unicode_text = "Hello 世界 🌍 مرحبا"

        await worker.process_request(
            request_id=8,
            module_name=module_name,
            function_name='handler_with_unicode',
            kwargs={'text': unicode_text}
        )

        response_data = worker.response_socket.send.call_args[0][0]
        parsed = self._parse_response(response_data)
        body = json.loads(parsed['body'])

        assert body['text'] == unicode_text
        assert body['length'] == len(unicode_text)

    @pytest.mark.asyncio
    async def test_process_many_requests_randomized(self, worker, test_handlers_module):
        """Test processing many requests with randomized data."""
        module_name, _ = test_handlers_module

        worker.response_socket = AsyncMock()

        num_requests = 50
        for i in range(num_requests):
            value = random.randint(-10000, 10000)
            name = ''.join(random.choices(string.ascii_letters, k=random.randint(5, 20)))

            await worker.process_request(
                request_id=i,
                module_name=module_name,
                function_name='handler_with_params',
                kwargs={'value': value, 'name': name}
            )

        assert worker.stats['requests_processed'] == num_requests
        assert worker.stats['requests_failed'] == 0
        assert worker.response_socket.send.call_count == num_requests

    def _parse_response(self, data):
        """Helper to parse response data."""
        header_format = '<BIIHIIB'  # 20 bytes
        header_size = struct.calcsize(header_format)

        header = struct.unpack_from(header_format, data, 0)

        msg_type = header[0]
        request_id = header[1]
        total_length = header[2]
        status_code = header[3]
        body_len = header[4]
        error_len = header[5]
        success = header[6]

        offset = header_size
        body = data[offset:offset + body_len].decode('utf-8')
        offset += body_len
        error = data[offset:offset + error_len].decode('utf-8')

        return {
            'type': msg_type,
            'request_id': request_id,
            'total_length': total_length,
            'status_code': status_code,
            'success': bool(success),
            'body': body,
            'error': error
        }


# =============================================================================
# Worker Statistics Tests
# =============================================================================

class TestWorkerStatistics:
    """Test worker statistics tracking."""

    def test_initial_stats(self, worker):
        """Test that initial stats are zero."""
        assert worker.stats['requests_processed'] == 0
        assert worker.stats['requests_failed'] == 0
        assert worker.stats['handlers_cached'] == 0

    @pytest.mark.asyncio
    async def test_stats_increment_on_success(self, worker, test_handlers_module):
        """Test stats increment on successful requests."""
        module_name, _ = test_handlers_module
        worker.response_socket = AsyncMock()

        for i in range(5):
            await worker.process_request(i, module_name, 'sync_handler', {})

        assert worker.stats['requests_processed'] == 5
        assert worker.stats['requests_failed'] == 0

    @pytest.mark.asyncio
    async def test_stats_increment_on_failure(self, worker, test_handlers_module):
        """Test stats increment on failed requests."""
        module_name, _ = test_handlers_module
        worker.response_socket = AsyncMock()

        for i in range(3):
            await worker.process_request(i, module_name, 'handler_that_raises', {})

        assert worker.stats['requests_processed'] == 0
        assert worker.stats['requests_failed'] == 3

    def test_stats_handlers_cached(self, worker, test_handlers_module):
        """Test handlers_cached stat."""
        module_name, _ = test_handlers_module

        worker.get_handler(module_name, 'sync_handler')
        worker.get_handler(module_name, 'async_handler')
        worker.get_handler(module_name, 'handler_with_params')

        # Calling same handler again shouldn't increment
        worker.get_handler(module_name, 'sync_handler')

        assert worker.stats['handlers_cached'] == 3


# =============================================================================
# IPC Path Configuration Tests
# =============================================================================

class TestIPCConfiguration:
    """Test IPC path configuration."""

    def test_ipc_paths_configured_correctly(self):
        """Test that IPC paths are configured based on prefix."""
        prefix = "test_prefix_123"
        worker = ZmqWorker(prefix, 1)

        assert worker.request_path == f"ipc:///tmp/{prefix}_req"
        assert worker.response_path == f"ipc:///tmp/{prefix}_resp"

    def test_worker_id_stored(self):
        """Test that worker ID is stored."""
        worker_id = random.randint(1, 1000)
        worker = ZmqWorker("test", worker_id)

        assert worker.worker_id == worker_id

    def test_randomized_prefix(self):
        """Test with randomized IPC prefix."""
        for _ in range(10):
            prefix = ''.join(random.choices(string.ascii_lowercase + string.digits, k=20))
            worker = ZmqWorker(prefix, 1)

            assert prefix in worker.request_path
            assert prefix in worker.response_path


# =============================================================================
# Edge Cases
# =============================================================================

class TestEdgeCases:
    """Test edge cases and boundary conditions."""

    def test_empty_module_name(self, worker):
        """Test with empty module name."""
        # Empty module name raises ValueError from importlib
        with pytest.raises((ImportError, ModuleNotFoundError, ValueError)):
            worker.get_handler('', 'func')

    def test_empty_function_name(self, worker, test_handlers_module):
        """Test with empty function name."""
        module_name, _ = test_handlers_module

        with pytest.raises(AttributeError):
            worker.get_handler(module_name, '')

    @pytest.mark.asyncio
    async def test_empty_kwargs_dict(self, worker, test_handlers_module):
        """Test processing with empty kwargs dict."""
        module_name, _ = test_handlers_module
        worker.response_socket = AsyncMock()

        await worker.process_request(1, module_name, 'sync_handler', {})

        assert worker.stats['requests_processed'] == 1

    def test_parse_request_exact_header_size(self, worker):
        """Test parsing message with exactly minimum header size."""
        message = b'\x01' * 21  # Exactly 21 bytes
        result = worker.parse_request(message)
        # Should parse but likely fail on string extraction
        # This tests boundary handling

    @pytest.mark.asyncio
    async def test_large_request_id(self, worker, test_handlers_module):
        """Test with maximum request ID value."""
        module_name, _ = test_handlers_module
        worker.response_socket = AsyncMock()

        large_id = 2**32 - 1  # Max uint32
        await worker.process_request(large_id, module_name, 'sync_handler', {})

        response_data = worker.response_socket.send.call_args[0][0]
        # Verify we can parse the response with large ID
        header = struct.unpack_from('<BIIIHIIB', response_data, 0)
        assert header[1] == large_id


# =============================================================================
# Module Entry Point Tests
# =============================================================================

class TestModuleEntryPoints:
    """Test module-level entry points."""

    def test_start_worker_function_exists(self):
        """Test that start_worker function is importable."""
        assert callable(start_worker)

    def test_worker_main_function_exists(self):
        """Test that worker_main function is importable."""
        assert asyncio.iscoroutinefunction(worker_main)

    def test_message_type_constants(self):
        """Test MessageType constants are defined."""
        assert MessageType.REQUEST == 1
        assert MessageType.RESPONSE == 2
        assert MessageType.SHUTDOWN == 3


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
