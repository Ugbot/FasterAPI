"""ZeroMQ-based worker process for executing Python handlers.

Each worker process uses ZeroMQ PULL/PUSH sockets to communicate with the
C++ master process. Supports both sync and async handlers.

This enables multi-language workers (Python, Go, Rust, etc.) via ZeroMQ.
"""

import asyncio
import sys
import os
import importlib
import inspect
import traceback
import logging
import json
import struct
from typing import Dict, Callable, Any, get_type_hints, get_origin, get_args, List, Optional, Union
from enum import Enum

# Import Request class for type injection
try:
    from fasterapi.http.request import Request
    _HAS_REQUEST_CLASS = True
except ImportError:
    _HAS_REQUEST_CLASS = False
    Request = None


def _is_request_type(param_type) -> bool:
    """Check if param_type is a Request type.

    Uses identity first (fast), then falls back to name/module check
    for cases where the same class is imported in different contexts.
    """
    if param_type is None:
        return False
    # Check by identity first (works in same process context)
    if _HAS_REQUEST_CLASS and param_type is Request:
        return True
    # Fallback: check by fully qualified name (handles cross-process imports)
    if hasattr(param_type, '__module__') and hasattr(param_type, '__name__'):
        return (param_type.__module__ == 'fasterapi.http.request' and
                param_type.__name__ == 'Request')
    return False

try:
    import zmq
    import zmq.asyncio
except ImportError:
    print("ERROR: pyzmq not installed. Install with: pip install pyzmq", file=sys.stderr)
    sys.exit(1)


# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='[ZMQ Worker %(process)d] %(levelname)s: %(message)s'
)
logger = logging.getLogger(__name__)


# Message types (must match C++ enum in ipc_protocol.h)
class MessageType:
    REQUEST = 1
    RESPONSE = 2
    SHUTDOWN = 3

    # WebSocket events
    WS_CONNECT = 10
    WS_MESSAGE = 11
    WS_DISCONNECT = 12

    # WebSocket responses from Python
    WS_SEND = 20
    WS_CLOSE = 21


# Payload format types (must match C++ PayloadFormat enum)
class PayloadFormat:
    FORMAT_JSON = 0
    FORMAT_BINARY_TLV = 1
    FORMAT_MSGPACK = 2


# Binary TLV magic byte
BINARY_KWARGS_MAGIC = 0xFA


# Try to import the Cython binary decoder, fall back to pure Python
try:
    from fasterapi.core.binary_kwargs import decode_kwargs as _decode_binary_kwargs, is_binary_format
    _HAS_CYTHON_DECODER = True
    logger.info("Using Cython binary_kwargs decoder")
except ImportError:
    _HAS_CYTHON_DECODER = False
    logger.info("Cython binary_kwargs not available, using JSON only")


# Lightweight proxy class for url_for support in ZMQ workers
# This provides the url_path_for method that Request.url_for() needs
class NamedRoutesProxy:
    """Lightweight proxy that provides url_path_for() for named route resolution.

    ZMQ workers run in separate processes and can't access the FastAPI app object.
    This proxy reads named routes from the FASTERAPI_NAMED_ROUTES env var and
    implements url_path_for() to enable request.url_for() functionality.
    """

    def __init__(self, named_routes: Dict[str, str]):
        """Initialize with a dict of {name: path_template}."""
        self._named_routes = named_routes

    def url_path_for(self, name: str, **path_params: Any) -> str:
        """Generate URL path for a named route (FastAPI-compatible)."""
        import re

        if name not in self._named_routes:
            raise LookupError(f"No route found with name '{name}'")

        path = self._named_routes[name]

        # Replace path parameters like {user_id} with actual values
        for param_name, param_value in path_params.items():
            placeholder = "{" + param_name + "}"
            if placeholder not in path:
                raise ValueError(f"Path parameter '{param_name}' not found in path '{path}'")
            path = path.replace(placeholder, str(param_value))

        # Verify no unreplaced parameters remain
        remaining = re.findall(r'\{(\w+)\}', path)
        if remaining:
            raise ValueError(f"Missing path parameters: {remaining}")

        return path


# Global named routes proxy - initialized from env var on worker startup
_named_routes_proxy: Optional[NamedRoutesProxy] = None


def _init_named_routes_proxy():
    """Initialize the global named routes proxy from environment variable."""
    global _named_routes_proxy

    named_routes_json = os.environ.get('FASTERAPI_NAMED_ROUTES')
    logger.info(f"FASTERAPI_NAMED_ROUTES env var: {named_routes_json[:100] if named_routes_json else 'NOT SET'}")
    if named_routes_json:
        try:
            named_routes = json.loads(named_routes_json)
            _named_routes_proxy = NamedRoutesProxy(named_routes)
            logger.info(f"Loaded {len(named_routes)} named routes for url_for support: {list(named_routes.keys())}")
        except json.JSONDecodeError as e:
            logger.warning(f"Failed to parse FASTERAPI_NAMED_ROUTES: {e}")
    else:
        logger.info("No named routes configured for url_for support")


def get_named_routes_proxy() -> Optional[NamedRoutesProxy]:
    """Get the global named routes proxy (may be None if no routes configured)."""
    return _named_routes_proxy


# Fallback functions when Cython decoder is not available
if not _HAS_CYTHON_DECODER:
    def is_binary_format(data: bytes) -> bool:
        """Check if data starts with binary TLV magic byte."""
        return len(data) >= 1 and data[0] == BINARY_KWARGS_MAGIC

    def _decode_binary_kwargs(data: bytes) -> dict:
        """Pure Python fallback for binary TLV decoding."""
        # Import the pure Python decoder from the Cython module
        try:
            from fasterapi.core.binary_kwargs import decode_kwargs_pure_python
            return decode_kwargs_pure_python(data)
        except ImportError:
            # Ultimate fallback - this shouldn't happen
            raise NotImplementedError("Binary kwargs decoder not available")


# =============================================================================
# Type coercion helpers for parameter resolution
# =============================================================================

class ValidationError(Exception):
    """Raised when type coercion fails - returns 422."""
    def __init__(self, errors: list):
        self.errors = errors
        super().__init__(f"Validation error: {errors}")


def coerce_value(value: Any, target_type: Any, param_name: str) -> tuple:
    """
    Coerce a parameter value to the target type.

    Returns:
        Tuple of (coerced_value, error_detail) where error_detail is None on success
    """
    # Handle None/empty
    if value is None:
        return None, None

    # Check if target_type is List[T]
    origin = get_origin(target_type)
    if origin is list:
        args = get_args(target_type)
        element_type = args[0] if args else str

        # Ensure value is a list
        if isinstance(value, list):
            values = value
        else:
            values = [value] if value else []

        # Coerce each element
        result = []
        for v in values:
            coerced, err = coerce_value(v, element_type, param_name)
            if err:
                return None, err
            result.append(coerced)
        return result, None

    # Handle Optional[T] - unwrap to T
    if origin is type(None):
        return None, None
    if str(origin) in ("typing.Union", "types.UnionType"):
        args = get_args(target_type)
        if len(args) == 2 and type(None) in args:
            # Optional[T] - get the non-None type
            inner = args[0] if args[1] is type(None) else args[1]
            if value is None or value == "":
                return None, None
            return coerce_value(value, inner, param_name)

    # If value is a list but we expect single, take last value
    if isinstance(value, list):
        value = value[-1] if value else ""

    # Already correct type? Return as-is
    if isinstance(value, target_type) if isinstance(target_type, type) else False:
        return value, None

    # Coerce string to target type
    try:
        if target_type == int:
            if isinstance(value, int):
                return value, None
            return int(value), None
        elif target_type == float:
            if isinstance(value, (int, float)):
                return float(value), None
            return float(value), None
        elif target_type == bool:
            if isinstance(value, bool):
                return value, None
            if isinstance(value, str):
                return value.lower() in ("true", "1", "yes", "on"), None
            return bool(value), None
        elif target_type == str:
            return str(value) if value is not None else "", None
        elif isinstance(target_type, type) and issubclass(target_type, Enum):
            return target_type(value), None
        else:
            # Default: return as-is
            return value, None
    except (ValueError, TypeError) as e:
        type_name = target_type.__name__ if hasattr(target_type, '__name__') else str(target_type)
        return None, {
            "loc": ["query", param_name],
            "msg": f"value is not a valid {type_name}",
            "type": "type_error",
            "input": value,
        }


class ZmqWorker:
    """ZeroMQ-based worker that executes Python handlers."""

    def __init__(self, ipc_prefix: str, worker_id: int):
        """Initialize ZMQ worker.

        Args:
            ipc_prefix: Prefix for IPC paths (e.g., "fasterapi_12345")
            worker_id: Unique ID for this worker (for logging)
        """
        self.ipc_prefix = ipc_prefix
        self.worker_id = worker_id
        self.context = None
        self.request_socket = None
        self.response_socket = None
        self.ws_socket = None  # Per-worker WebSocket channel
        self.module_cache: Dict[str, Any] = {}
        self.function_cache: Dict[tuple, Callable] = {}
        self.stats = {
            'requests_processed': 0,
            'requests_failed': 0,
            'handlers_cached': 0,
            'ws_messages_processed': 0
        }

        # IPC paths
        self.request_path = f"ipc:///tmp/{ipc_prefix}_req"
        self.response_path = f"ipc:///tmp/{ipc_prefix}_resp"
        # Per-worker WebSocket channel (for WS_MESSAGE/WS_DISCONNECT with affinity)
        self.ws_path = f"ipc:///tmp/{ipc_prefix}_ws_{worker_id}"

        # WebSocket handler registry: path -> handler
        self.ws_handlers: Dict[str, Callable] = {}

        # Load pre-serialized WebSocket handlers from environment (for __main__ handlers)
        self._load_ws_handlers_from_env()

        # Active WebSocket connections: connection_id -> WebSocketProxy
        self.ws_connections: Dict[int, "WebSocketProxy"] = {}

    async def run(self):
        """Main worker loop. Receives requests via ZMQ and executes handlers."""
        logger.info(f"Worker {self.worker_id} starting with ZeroMQ")
        logger.info(f"Request path:  {self.request_path}")
        logger.info(f"Response path: {self.response_path}")
        logger.info(f"WebSocket path: {self.ws_path}")

        # Create ZMQ context
        self.context = zmq.asyncio.Context()

        # Connect sockets
        try:
            # PULL socket for requests (HTTP + WS_CONNECT via round-robin)
            self.request_socket = self.context.socket(zmq.PULL)
            self.request_socket.connect(self.request_path)

            # PUSH socket for responses
            self.response_socket = self.context.socket(zmq.PUSH)
            self.response_socket.connect(self.response_path)

            # PULL socket for per-worker WebSocket events (WS_MESSAGE/WS_DISCONNECT with affinity)
            self.ws_socket = self.context.socket(zmq.PULL)
            # Connect with retry - master may not have created channels yet
            ws_connected = False
            for retry in range(10):
                try:
                    self.ws_socket.connect(self.ws_path)
                    ws_connected = True
                    logger.info(f"Worker {self.worker_id} connected to per-worker WS channel")
                    break
                except Exception as e:
                    logger.debug(f"WS socket connect retry {retry}: {e}")
                    await asyncio.sleep(0.1)
            if not ws_connected:
                logger.warning(f"Worker {self.worker_id} could not connect to WS channel, WS affinity disabled")
                self.ws_socket.close()
                self.ws_socket = None

            logger.info(f"Worker {self.worker_id} connected successfully")

        except Exception as e:
            logger.error(f"Failed to connect sockets: {e}")
            return

        # Main request processing loop using poll for multiple sockets
        poller = zmq.asyncio.Poller()
        poller.register(self.request_socket, zmq.POLLIN)
        if self.ws_socket:
            poller.register(self.ws_socket, zmq.POLLIN)

        while True:
            try:
                # Poll for messages from any socket (100ms timeout)
                socks = dict(await poller.poll(timeout=100))

                # Check request socket (HTTP requests + WS_CONNECT)
                if self.request_socket in socks:
                    message = await self.request_socket.recv()

                    if len(message) < 1:
                        continue

                    msg_type = message[0]

                    # Handle WebSocket events from shared request queue
                    if msg_type in (MessageType.WS_CONNECT, MessageType.WS_MESSAGE, MessageType.WS_DISCONNECT):
                        await self.process_ws_event(message)
                        continue

                    # Parse HTTP request
                    request_data = self.parse_request(message)

                    if request_data is None:
                        # Shutdown signal
                        logger.info(f"Worker {self.worker_id} received shutdown signal")
                        break

                    request_id, module_name, function_name, kwargs = request_data
                    logger.debug(f"Processing request {request_id}: {module_name}.{function_name}")

                    # Execute handler
                    await self.process_request(request_id, module_name, function_name, kwargs)

                # Check per-worker WebSocket socket (WS_CONNECT/WS_MESSAGE/WS_DISCONNECT with affinity)
                if self.ws_socket and self.ws_socket in socks:
                    message = await self.ws_socket.recv()

                    if len(message) >= 1:
                        msg_type = message[0]
                        if msg_type in (MessageType.WS_CONNECT, MessageType.WS_MESSAGE, MessageType.WS_DISCONNECT):
                            await self.process_ws_event(message)
                        else:
                            logger.warning(f"Unexpected message type {msg_type} on WS socket")

            except Exception as e:
                logger.error(f"Error in main loop: {e}")
                logger.error(traceback.format_exc())

        # Cleanup
        logger.info(f"Worker {self.worker_id} shutting down. Stats: {self.stats}")
        if self.request_socket:
            self.request_socket.close()
        if self.response_socket:
            self.response_socket.close()
        if self.ws_socket:
            self.ws_socket.close()
        if self.context:
            self.context.term()

    def _load_ws_handlers_from_env(self):
        """Load pre-serialized WebSocket handlers from environment variable.

        When handlers are defined in __main__ (like in a script running via
        app.run()), they cannot be imported normally in forked workers. Instead,
        the parent process serializes them with cloudpickle and passes via env.
        """
        import base64 as base64_module

        encoded = os.environ.get("FASTERAPI_WS_HANDLERS")
        if not encoded:
            return

        try:
            import cloudpickle

            pickled = base64_module.b64decode(encoded)
            handlers = cloudpickle.loads(pickled)

            if handlers and isinstance(handlers, dict):
                self.ws_handlers.update(handlers)
                logger.info(f"Loaded {len(handlers)} WebSocket handlers from env")
                for path in handlers:
                    logger.debug(f"  Handler for: {path}")
        except ImportError:
            logger.warning("cloudpickle not installed, cannot deserialize WS handlers")
        except Exception as e:
            logger.error(f"Failed to deserialize WebSocket handlers: {e}")

    def parse_request(self, data: bytes):
        """Parse request message from C++.

        Message format (must match C++ MessageHeader):
        - type (uint8): MessageType
        - request_id (uint32)
        - total_length (uint32)
        - module_name_len (uint32)
        - function_name_len (uint32)
        - kwargs_len (uint32)
        - kwargs_format (uint8): PayloadFormat
        - module_name (string)
        - function_name (string)
        - kwargs_data (bytes): JSON string or binary TLV
        """
        if len(data) < 22:  # Minimum header size (B + 5*I + B = 22 bytes)
            return None

        # Parse header (22 bytes total): B + 5*I + B = 1 + 20 + 1 = 22 bytes
        header_format = '<BIIIIIB'  # Little-endian: byte + 5 uint32s + byte
        header = struct.unpack_from(header_format, data, 0)

        msg_type = header[0]
        request_id = header[1]
        # header[2] is total_length (not used directly)
        module_name_len = header[3]
        function_name_len = header[4]
        kwargs_len = header[5]
        kwargs_format = header[6]

        # Check for shutdown
        if msg_type == MessageType.SHUTDOWN:
            return None

        # Extract strings
        offset = 22
        module_name = data[offset:offset + module_name_len].decode('utf-8')
        offset += module_name_len

        function_name = data[offset:offset + function_name_len].decode('utf-8')
        offset += function_name_len

        # Extract kwargs data (format-agnostic)
        kwargs_data = data[offset:offset + kwargs_len]

        # Decode kwargs based on format
        kwargs = self._decode_kwargs(kwargs_data, kwargs_format)

        return (request_id, module_name, function_name, kwargs)

    def _decode_kwargs(self, data: bytes, format_hint: int) -> dict:
        """Decode kwargs from binary or JSON format.

        Args:
            data: Raw kwargs bytes (JSON string, binary TLV, or MessagePack)
            format_hint: PayloadFormat value from header

        Returns:
            Decoded kwargs dict
        """
        if not data:
            return {}

        # Check for binary TLV format (magic byte 0xFA)
        if len(data) >= 1 and data[0] == BINARY_KWARGS_MAGIC:
            # Binary TLV format (~26x faster than JSON)
            try:
                return _decode_binary_kwargs(data)
            except Exception as e:
                logger.warning(f"Binary kwargs decode failed, falling back to JSON: {e}")
                # Fall through to JSON decode

        # Check format hint from header
        if format_hint == PayloadFormat.FORMAT_BINARY_TLV:
            # Header says binary but magic byte missing - try anyway
            try:
                return _decode_binary_kwargs(data)
            except Exception:
                pass  # Fall through to JSON

        if format_hint == PayloadFormat.FORMAT_MSGPACK:
            # MessagePack format (not yet implemented on Python side)
            # TODO: Add msgpack decoding when complex types need it
            logger.warning("MessagePack format not yet supported, trying JSON")

        # Default: JSON format
        try:
            kwargs_str = data.decode('utf-8')
            return json.loads(kwargs_str) if kwargs_str else {}
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            logger.error(f"Failed to decode kwargs: {e}")
            return {}

    def serialize_response(self, request_id: int, status_code: int, success: bool,
                          body_json: str, error_message: str = "") -> bytes:
        """Serialize response message for C++.

        Response format (must match C++ ResponseHeader):
        - type (uint8): MessageType::RESPONSE
        - request_id (uint32)
        - total_length (uint32)
        - status_code (uint16)
        - body_json_len (uint32)
        - error_message_len (uint32)
        - success (uint8)
        - body_json (string)
        - error_message (string)
        """
        body_bytes = body_json.encode('utf-8')
        error_bytes = error_message.encode('utf-8')

        # Header: B + I + I + H + I + I + B = 1+4+4+2+4+4+1 = 20 bytes
        header_size = 20
        total_length = header_size + len(body_bytes) + len(error_bytes)

        # Pack header (20 bytes total)
        header = struct.pack(
            '<BIIHIIB',  # Little-endian: B(type) I(req_id) I(total_len) H(status) I(body_len) I(error_len) B(success)
            MessageType.RESPONSE,  # type
            request_id,            # request_id
            total_length,          # total_length
            status_code,           # status_code (uint16)
            len(body_bytes),       # body_json_len
            len(error_bytes),      # error_message_len
            1 if success else 0    # success (uint8)
        )

        return header + body_bytes + error_bytes

    def resolve_handler_params(self, handler: Callable, kwargs: dict, module_name: str = None) -> dict:
        """
        Resolve handler parameters with type coercion and Request injection.

        Args:
            handler: The handler function
            kwargs: Raw kwargs from C++
            module_name: Name of the module containing the handler (for app lookup)

        Returns:
            Resolved kwargs with proper types

        Raises:
            ValidationError: If type coercion fails
        """
        # Unwrap decorated functions to get the original function for type inspection
        # The @wraps decorator sets __wrapped__ on wrapper functions
        original_handler = handler
        while hasattr(original_handler, '__wrapped__'):
            original_handler = original_handler.__wrapped__

        # Debug: Log entry into this function
        logger.info(f"[URL_FOR_DEBUG] resolve_handler_params CALLED for {handler.__name__}, original={original_handler.__name__}, kwargs keys: {list(kwargs.keys())}")

        # Get type hints for the handler
        # Always filter out internal keys that start with __ (like __request_data__)
        filtered_kwargs = {k: v for k, v in kwargs.items() if not k.startswith("__")}

        try:
            # Use original_handler for type hints since wrapper may not have them in its namespace
            hints = get_type_hints(original_handler)
            logger.info(f"[URL_FOR_DEBUG] get_type_hints for {original_handler.__name__}: {hints}")
        except Exception as e:
            # If we can't get type hints, log it and return filtered kwargs
            logger.warning(f"[URL_FOR_DEBUG] get_type_hints failed for {original_handler.__name__}: {type(e).__name__}: {e}")
            return filtered_kwargs

        if not hints:
            return filtered_kwargs

        # Use original_handler for signature since wrapper has *args, **kw
        sig = inspect.signature(original_handler)
        resolved_kwargs = {}
        validation_errors = []

        for param_name, param in sig.parameters.items():
            # Get the type hint
            param_type = hints.get(param_name)
            if param_type is None:
                # No type hint - pass through as-is
                if param_name in kwargs:
                    resolved_kwargs[param_name] = kwargs[param_name]
                continue

            # Debug: Log all type hints being processed
            is_request = _is_request_type(param_type)
            logger.info(f"[URL_FOR_DEBUG] Checking param '{param_name}': type={param_type}, is_request={is_request}")

            # Check if this is a Request type
            if is_request:
                # Create a Request object from the available data
                # Extract request data from special kwargs if present
                request_data = kwargs.get("__request_data__", {})
                logger.info(f"[URL_FOR_DEBUG] Request injection: request_data keys = {list(request_data.keys())}")

                # Build scope with app reference for url_for support
                scope = request_data.get("scope", {})
                if "app" not in scope or scope.get("app") is None:
                    # First try to get the real app from the module
                    app = self._get_app_for_module(module_name) if module_name else None
                    if app:
                        scope = dict(scope)  # Don't modify original
                        scope["app"] = app
                        logger.info(f"[URL_FOR_DEBUG] Request injection: using app from module")
                    else:
                        # Fall back to named routes proxy for url_for support
                        proxy = get_named_routes_proxy()
                        logger.info(f"[URL_FOR_DEBUG] Request injection: proxy = {proxy}, named_routes = {proxy._named_routes if proxy else 'N/A'}")
                        if proxy:
                            scope = dict(scope)  # Don't modify original
                            scope["app"] = proxy
                            logger.info(f"[URL_FOR_DEBUG] Request injection: added proxy to scope, scope['app'] = {scope.get('app')}")

                request = Request(
                    method=request_data.get("method", "GET"),
                    path=request_data.get("path", "/"),
                    query=request_data.get("query", ""),
                    headers=request_data.get("headers", {}),
                    query_params=request_data.get("query_params", {}),
                    path_params=request_data.get("path_params", {}),
                    body=request_data.get("body", ""),
                    client_ip=request_data.get("client_ip", "127.0.0.1"),
                    client_port=request_data.get("client_port", 0),
                    scope=scope,
                )
                resolved_kwargs[param_name] = request
                continue

            # Get value from kwargs
            value = kwargs.get(param_name)

            # Check if param has a default
            has_default = param.default is not inspect.Parameter.empty
            default_value = param.default if has_default else None

            if value is None:
                # No value provided - use default if available
                if has_default:
                    resolved_kwargs[param_name] = default_value
                else:
                    # Check if Optional type
                    origin = get_origin(param_type)
                    if str(origin) in ("typing.Union", "types.UnionType"):
                        args = get_args(param_type)
                        if type(None) in args:
                            resolved_kwargs[param_name] = None
                            continue
                    # Required param missing - this shouldn't happen if C++ validates
                    resolved_kwargs[param_name] = None
                continue

            # Coerce value to target type
            coerced_value, error = coerce_value(value, param_type, param_name)
            if error:
                validation_errors.append(error)
            else:
                resolved_kwargs[param_name] = coerced_value

        # If there were validation errors, raise
        if validation_errors:
            raise ValidationError(validation_errors)

        return resolved_kwargs

    async def process_request(self, request_id: int, module_name: str,
                             function_name: str, kwargs: dict):
        """Process a single request by executing the handler.

        Args:
            request_id: Unique request ID
            module_name: Python module containing the handler
            function_name: Name of the handler function
            kwargs: Arguments to pass to the handler
        """
        try:
            # Get handler function
            handler = self.get_handler(module_name, function_name)

            logger.debug(f"Handler type: {type(handler)}, is_coroutine: {inspect.iscoroutinefunction(handler)}")
            logger.debug(f"Handler: {handler}")
            logger.debug(f"Kwargs: {kwargs}")

            # Resolve handler parameters (type coercion, Request injection)
            try:
                resolved_kwargs = self.resolve_handler_params(handler, kwargs, module_name)
            except ValidationError as ve:
                # Return 422 Validation Error
                error_body = json.dumps({"detail": ve.errors})
                response_data = self.serialize_response(
                    request_id,
                    422,  # HTTP 422 Unprocessable Entity
                    False,  # error
                    error_body,
                    "Validation Error"
                )
                await self.response_socket.send(response_data)
                self.stats['requests_failed'] += 1
                return

            # Execute handler (sync or async)
            if inspect.iscoroutinefunction(handler):
                logger.debug("Calling async handler")
                result = await handler(**resolved_kwargs)
            else:
                logger.debug("Calling sync handler via to_thread")
                # Call the handler directly - it's sync
                result = handler(**resolved_kwargs)

            logger.debug(f"Handler result type: {type(result)}")
            logger.debug(f"Handler result: {result}")

            # Check if result is a coroutine that wasn't awaited
            if inspect.iscoroutine(result):
                logger.error(f"Handler returned coroutine object! This shouldn't happen. Handler: {handler}")
                # Try to await it
                result = await result

            # Serialize result to JSON
            body_json = json.dumps(result)

            # Send response
            response_data = self.serialize_response(
                request_id,
                200,  # HTTP 200 OK
                True,  # success
                body_json
            )
            await self.response_socket.send(response_data)

            self.stats['requests_processed'] += 1

        except Exception as e:
            # Handle errors
            error_msg = f"{type(e).__name__}: {str(e)}"
            error_trace = traceback.format_exc()
            logger.error(f"Handler error for request {request_id}: {error_msg}")
            logger.debug(error_trace)

            # Send error response
            error_data = json.dumps({"error": error_msg, "traceback": error_trace})
            response_data = self.serialize_response(
                request_id,
                500,  # HTTP 500 Internal Server Error
                False,  # error
                error_data,
                error_msg
            )
            await self.response_socket.send(response_data)

            self.stats['requests_failed'] += 1

    def get_handler(self, module_name: str, function_name: str) -> Callable:
        """Get handler function, using cache if available.

        Args:
            module_name: Python module containing the handler
            function_name: Name of the handler function

        Returns:
            Callable handler function

        Raises:
            ImportError: If module cannot be imported
            AttributeError: If function not found in module
        """
        cache_key = (module_name, function_name)

        # Check cache first
        if cache_key in self.function_cache:
            return self.function_cache[cache_key]

        # Import module (with caching)
        if module_name in self.module_cache:
            module = self.module_cache[module_name]
        else:
            logger.debug(f"Importing module: {module_name}")
            module = importlib.import_module(module_name)
            self.module_cache[module_name] = module

            # Try to find the app object in the module for url_for support
            self._find_and_cache_app(module, module_name)

        # Get function from module
        if not hasattr(module, function_name):
            raise AttributeError(f"Module '{module_name}' has no function '{function_name}'")

        handler = getattr(module, function_name)

        # Cache it
        self.function_cache[cache_key] = handler
        self.stats['handlers_cached'] += 1

        logger.debug(f"Cached handler: {module_name}.{function_name}")

        return handler

    def _find_and_cache_app(self, module, module_name: str):
        """Find FastAPI app object in module for url_for support.

        Looks for common app variable names and caches the first one found
        that has url_path_for method.
        """
        if not hasattr(self, '_app_cache'):
            self._app_cache = {}

        # Already have an app for this module?
        if module_name in self._app_cache:
            return

        # Common names for FastAPI app instances
        app_names = ['app', 'application', 'api', 'fastapi', 'server']

        for name in app_names:
            if hasattr(module, name):
                candidate = getattr(module, name)
                # Check if it looks like a FastAPI app (has url_path_for)
                if hasattr(candidate, 'url_path_for') and hasattr(candidate, '_named_routes'):
                    self._app_cache[module_name] = candidate
                    logger.debug(f"Found app '{name}' in module {module_name} for url_for support")
                    return

        # Fallback: scan all module attributes
        for name in dir(module):
            if name.startswith('_'):
                continue
            try:
                candidate = getattr(module, name)
                if hasattr(candidate, 'url_path_for') and hasattr(candidate, '_named_routes'):
                    self._app_cache[module_name] = candidate
                    logger.debug(f"Found app '{name}' in module {module_name} for url_for support")
                    return
            except Exception:
                pass

    def _get_app_for_module(self, module_name: str):
        """Get cached app object for a module."""
        if hasattr(self, '_app_cache'):
            return self._app_cache.get(module_name)
        return None

    # =========================================================================
    # WebSocket handling
    # =========================================================================

    def parse_ws_message(self, data: bytes):
        """Parse WebSocket message from C++.

        WebSocketMessageHeader format (must match C++):
        - type (uint8): MessageType
        - connection_id (uint64)
        - total_length (uint32)
        - path_len (uint32)
        - payload_len (uint32)
        - is_binary (uint8)
        - path (string)
        - payload (string/bytes)
        """
        if len(data) < 22:  # Minimum header size
            return None

        # Header: B + Q + 3*I + B = 1 + 8 + 12 + 1 = 22 bytes
        header_format = '<BQIIIB'
        header = struct.unpack_from(header_format, data, 0)

        msg_type = header[0]
        connection_id = header[1]
        path_len = header[3]
        payload_len = header[4]
        is_binary = header[5] != 0

        # Extract path and payload
        offset = 22
        path = data[offset:offset + path_len].decode('utf-8') if path_len > 0 else ""
        offset += path_len

        if is_binary:
            payload = data[offset:offset + payload_len]
        else:
            payload = data[offset:offset + payload_len].decode('utf-8') if payload_len > 0 else ""

        return (msg_type, connection_id, path, payload, is_binary)

    async def process_ws_event(self, data: bytes):
        """Process WebSocket event from C++."""
        try:
            parsed = self.parse_ws_message(data)
            if parsed is None:
                logger.error("Failed to parse WebSocket message")
                return

            msg_type, connection_id, path, payload, is_binary = parsed

            if msg_type == MessageType.WS_CONNECT:
                await self.handle_ws_connect(connection_id, path, payload)
            elif msg_type == MessageType.WS_MESSAGE:
                await self.handle_ws_message(connection_id, path, payload, is_binary)
            elif msg_type == MessageType.WS_DISCONNECT:
                await self.handle_ws_disconnect(connection_id, path)

            self.stats['ws_messages_processed'] += 1

        except Exception as e:
            logger.error(f"Error processing WebSocket event: {e}")
            logger.error(traceback.format_exc())

    async def handle_ws_connect(self, connection_id: int, path: str, metadata_json: str = ""):
        """Handle WebSocket connection opened.

        Args:
            connection_id: Unique connection ID
            path: WebSocket path
            metadata_json: JSON string with module and function name
        """
        logger.debug(f"WebSocket connect: conn={connection_id} path={path} metadata={metadata_json}")

        # Create proxy for this connection
        proxy = WebSocketProxy(connection_id, path, self.response_socket)
        self.ws_connections[connection_id] = proxy

        # Try to get handler from metadata (module.function import)
        handler = None

        if metadata_json:
            try:
                metadata = json.loads(metadata_json)
                module_name = metadata.get("module")
                function_name = metadata.get("function")
                file_path = metadata.get("file")

                if module_name and function_name:
                    logger.info(f"Importing WebSocket handler: {module_name}.{function_name}")

                    module = None

                    # Handle __main__ module by loading from file
                    if module_name == '__main__' and file_path:
                        import importlib.util as importlib_util
                        logger.info(f"Loading __main__ module from file: {file_path}")
                        spec = importlib_util.spec_from_file_location("__main_app__", file_path)
                        if spec and spec.loader:
                            module = importlib_util.module_from_spec(spec)
                            spec.loader.exec_module(module)
                    else:
                        # Standard import
                        module = importlib.import_module(module_name)

                    if module:
                        # Get the function (handle nested names like Class.method)
                        obj = module
                        for part in function_name.split('.'):
                            obj = getattr(obj, part)
                        handler = obj

                        # Cache for future connections
                        self.ws_handlers[path] = handler
            except Exception as e:
                logger.error(f"Failed to import WebSocket handler: {e}")
                logger.error(traceback.format_exc())

        # Fall back to pre-registered handler
        if handler is None:
            handler = self.ws_handlers.get(path)

        if handler:
            # Start the async handler
            asyncio.create_task(self._run_ws_handler(handler, proxy))
        else:
            logger.warn(f"No WebSocket handler for path: {path}")

    async def handle_ws_message(self, connection_id: int, path: str, payload, is_binary: bool):
        """Handle WebSocket message received."""
        logger.debug(f"WebSocket message: conn={connection_id} path={path} len={len(payload)} binary={is_binary}")

        # Get connection proxy
        proxy = self.ws_connections.get(connection_id)
        if proxy:
            # Queue the message for the handler
            await proxy._receive_queue.put((payload, is_binary))

    async def handle_ws_disconnect(self, connection_id: int, path: str):
        """Handle WebSocket connection closed."""
        logger.debug(f"WebSocket disconnect: conn={connection_id} path={path}")

        proxy = self.ws_connections.get(connection_id)
        if proxy:
            # Signal close to the handler
            proxy._closed = True
            await proxy._receive_queue.put((None, False))  # Sentinel
            del self.ws_connections[connection_id]

    async def _run_ws_handler(self, handler: Callable, proxy: "WebSocketProxy"):
        """Run a WebSocket handler coroutine."""
        try:
            if asyncio.iscoroutinefunction(handler):
                await handler(proxy)
            else:
                handler(proxy)
        except Exception as e:
            logger.error(f"WebSocket handler error: {e}")
            logger.error(traceback.format_exc())
        finally:
            if not proxy._closed:
                await proxy.close()

    def register_ws_handler(self, path: str, handler: Callable):
        """Register a WebSocket handler for a path."""
        self.ws_handlers[path] = handler
        logger.info(f"Registered WebSocket handler for {path}")


class WebSocketProxy:
    """Proxy object for WebSocket connections in worker processes.

    Provides async send/receive interface to handlers while
    communicating with C++ server via ZMQ.
    """

    def __init__(self, connection_id: int, path: str, response_socket):
        self.connection_id = connection_id
        self.path = path
        self._response_socket = response_socket
        self._receive_queue: asyncio.Queue = asyncio.Queue()
        self._closed = False

    async def accept(self, subprotocol: Optional[str] = None) -> None:
        """Accept the WebSocket connection.

        This is a no-op since the C++ server already completed the HTTP 101
        handshake before sending WS_CONNECT to Python. We keep this method
        for API compatibility with FastAPI's WebSocket class.
        """
        pass

    async def send(self, message):
        """Send message (auto-detect type)."""
        if isinstance(message, bytes):
            await self.send_binary(message)
        else:
            await self.send_text(str(message))

    async def send_text(self, text: str):
        """Send text message."""
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        # Build WS_SEND response
        response = self._serialize_ws_response(
            MessageType.WS_SEND,
            text.encode('utf-8'),
            is_binary=False
        )
        await self._response_socket.send(response)

    async def send_binary(self, data: bytes):
        """Send binary message."""
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        response = self._serialize_ws_response(
            MessageType.WS_SEND,
            data,
            is_binary=True
        )
        await self._response_socket.send(response)

    async def receive(self):
        """Receive next message from client."""
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        payload, is_binary = await self._receive_queue.get()

        if payload is None:
            raise RuntimeError("WebSocket connection closed")

        return payload

    async def receive_text(self) -> str:
        """Receive text message."""
        msg = await self.receive()
        if isinstance(msg, bytes):
            return msg.decode('utf-8')
        return msg

    async def receive_binary(self) -> bytes:
        """Receive binary message."""
        msg = await self.receive()
        if isinstance(msg, str):
            return msg.encode('utf-8')
        return msg

    async def close(self, code: int = 1000, reason: str = ""):
        """Close the WebSocket connection."""
        if self._closed:
            return

        self._closed = True

        # Build WS_CLOSE response
        response = self._serialize_ws_response(
            MessageType.WS_CLOSE,
            b"",
            is_binary=False,
            close_code=code
        )
        await self._response_socket.send(response)

    @property
    def is_open(self) -> bool:
        """Check if connection is open."""
        return not self._closed

    def _serialize_ws_response(self, msg_type: int, payload: bytes,
                                is_binary: bool, close_code: int = 0) -> bytes:
        """Serialize WebSocket response for C++.

        WebSocketResponseHeader format:
        - type (uint8)
        - connection_id (uint64)
        - total_length (uint32)
        - payload_len (uint32)
        - close_code (uint16)
        - is_binary (uint8)
        - payload (bytes)
        """
        header_size = 20  # 1 + 8 + 4 + 4 + 2 + 1 = 20
        total_length = header_size + len(payload)

        # Pack header: B + Q + I + I + H + B = 1+8+4+4+2+1 = 20 bytes
        header = struct.pack(
            '<BQIIHB',
            msg_type,
            self.connection_id,
            total_length,
            len(payload),
            close_code,
            1 if is_binary else 0
        )

        return header + payload


async def worker_main(ipc_prefix: str, worker_id: int):
    """Entry point for ZMQ worker process.

    Args:
        ipc_prefix: IPC prefix
        worker_id: Worker ID
    """
    worker = ZmqWorker(ipc_prefix, worker_id)
    await worker.run()


def start_worker(ipc_prefix: str, worker_id: int):
    """Start a ZMQ worker process (synchronous entry point).

    Args:
        ipc_prefix: IPC prefix
        worker_id: Worker ID
    """
    # Run asyncio event loop
    asyncio.run(worker_main(ipc_prefix, worker_id))


if __name__ == "__main__":
    # Entry point when launched directly
    if len(sys.argv) < 3:
        print("Usage: python -m fasterapi.core.zmq_worker <ipc_prefix> <worker_id>")
        sys.exit(1)

    ipc_prefix = sys.argv[1]
    worker_id = int(sys.argv[2])

    # Add project directory to path if specified
    project_dir = os.environ.get('FASTERAPI_PROJECT_DIR')
    if project_dir and project_dir not in sys.path:
        sys.path.insert(0, project_dir)

    # Initialize named routes proxy for url_for support
    _init_named_routes_proxy()

    logger.info(f"Starting ZMQ worker {worker_id} with IPC prefix: {ipc_prefix}")
    logger.info(f"Python path: {sys.path[:3]}")  # Log first 3 entries

    try:
        start_worker(ipc_prefix, worker_id)
    except KeyboardInterrupt:
        logger.info(f"Worker {worker_id} interrupted")
    except Exception as e:
        logger.error(f"Worker {worker_id} failed: {e}")
        logger.error(traceback.format_exc())
        sys.exit(1)
