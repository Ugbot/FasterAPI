"""
FastAPI-compatible WebSocket wrapper for FasterAPI.

Provides WebSocket class and WebSocketDisconnect exception.
"""

import asyncio
import enum
import json
from typing import Any, Dict, Iterable, Optional, Union


class WebSocketState(enum.Enum):
    """WebSocket connection state."""

    CONNECTING = 0
    CONNECTED = 1
    DISCONNECTED = 2


class WebSocketDisconnect(Exception):
    """
    Exception raised when a WebSocket connection is closed.

    Usage:
        @app.websocket("/ws")
        async def websocket_endpoint(websocket: WebSocket):
            await websocket.accept()
            try:
                while True:
                    data = await websocket.receive_text()
                    await websocket.send_text(f"Echo: {data}")
            except WebSocketDisconnect:
                print("Client disconnected")
    """

    def __init__(self, code: int = 1000, reason: Optional[str] = None) -> None:
        self.code = code
        self.reason = reason


class WebSocket:
    """
    FastAPI-compatible WebSocket wrapper.

    This wraps the underlying C++ WebSocket implementation to provide
    an API matching FastAPI's WebSocket class.

    Usage:
        @app.websocket("/ws/{client_id}")
        async def websocket_endpoint(
            websocket: WebSocket,
            client_id: str,
        ):
            await websocket.accept()
            await websocket.send_json({"message": "Connected", "client_id": client_id})

            try:
                while True:
                    data = await websocket.receive_json()
                    await websocket.send_json({"echo": data})
            except WebSocketDisconnect:
                print(f"Client {client_id} disconnected")
    """

    def __init__(
        self,
        scope: Dict[str, Any],
        receive: Any = None,
        send: Any = None,
    ) -> None:
        """
        Initialize WebSocket wrapper.

        Args:
            scope: ASGI scope dictionary or native WebSocket object
            receive: ASGI receive callable (optional)
            send: ASGI send callable (optional)
        """
        self._scope = scope
        self._receive = receive
        self._send = send
        self._state = WebSocketState.CONNECTING

        # If scope is actually a native WebSocket object, wrap it
        if hasattr(scope, "send") and hasattr(scope, "receive"):
            self._native_ws = scope
            self._scope = {}
        else:
            self._native_ws = None

        # Extract path params and query params from scope
        self._path_params: Dict[str, str] = (
            scope.get("path_params", {}) if isinstance(scope, dict) else {}
        )
        self._query_params: Dict[str, str] = (
            scope.get("query_string", {}) if isinstance(scope, dict) else {}
        )
        self._headers: Dict[str, str] = {}

        if isinstance(scope, dict):
            # Parse headers from scope
            headers_list = scope.get("headers", [])
            for key, value in headers_list:
                if isinstance(key, bytes):
                    key = key.decode("latin1")
                if isinstance(value, bytes):
                    value = value.decode("latin1")
                self._headers[key.lower()] = value

    @property
    def client(self) -> Optional[tuple]:
        """Get client address."""
        if isinstance(self._scope, dict):
            return self._scope.get("client")
        if self._native_ws and hasattr(self._native_ws, "client"):
            return self._native_ws.client
        return None

    @property
    def headers(self) -> Dict[str, str]:
        """Get request headers."""
        return self._headers

    @property
    def path_params(self) -> Dict[str, str]:
        """Get path parameters."""
        return self._path_params

    @property
    def query_params(self) -> Dict[str, str]:
        """Get query parameters."""
        return self._query_params

    @property
    def state(self) -> WebSocketState:
        """Get current connection state."""
        return self._state

    async def accept(
        self,
        subprotocol: Optional[str] = None,
        headers: Optional[Iterable[tuple]] = None,
    ) -> None:
        """
        Accept the WebSocket connection.

        Must be called before sending or receiving messages.
        """
        if self._state != WebSocketState.CONNECTING:
            raise RuntimeError(f"Cannot accept in state {self._state}")

        if self._native_ws:
            # Native WebSocket
            if hasattr(self._native_ws, "accept"):
                if asyncio.iscoroutinefunction(self._native_ws.accept):
                    await self._native_ws.accept(subprotocol=subprotocol)
                else:
                    self._native_ws.accept(subprotocol=subprotocol)
        elif self._send:
            # ASGI interface
            message: Dict[str, Any] = {"type": "websocket.accept"}
            if subprotocol:
                message["subprotocol"] = subprotocol
            if headers:
                message["headers"] = list(headers)
            await self._send(message)

        self._state = WebSocketState.CONNECTED

    async def close(
        self,
        code: int = 1000,
        reason: Optional[str] = None,
    ) -> None:
        """Close the WebSocket connection."""
        if self._state == WebSocketState.DISCONNECTED:
            return

        if self._native_ws:
            if hasattr(self._native_ws, "close"):
                if asyncio.iscoroutinefunction(self._native_ws.close):
                    await self._native_ws.close(code=code, reason=reason)
                else:
                    self._native_ws.close(code=code, reason=reason)
        elif self._send:
            message: Dict[str, Any] = {
                "type": "websocket.close",
                "code": code,
            }
            if reason:
                message["reason"] = reason
            await self._send(message)

        self._state = WebSocketState.DISCONNECTED

    async def send_text(self, data: str) -> None:
        """Send a text message."""
        if self._state != WebSocketState.CONNECTED:
            raise RuntimeError(f"Cannot send in state {self._state}")

        if self._native_ws:
            if hasattr(self._native_ws, "send_text"):
                if asyncio.iscoroutinefunction(self._native_ws.send_text):
                    await self._native_ws.send_text(data)
                else:
                    self._native_ws.send_text(data)
            elif hasattr(self._native_ws, "send"):
                if asyncio.iscoroutinefunction(self._native_ws.send):
                    await self._native_ws.send(data)
                else:
                    self._native_ws.send(data)
        elif self._send:
            await self._send(
                {
                    "type": "websocket.send",
                    "text": data,
                }
            )

    async def send_bytes(self, data: bytes) -> None:
        """Send a binary message."""
        if self._state != WebSocketState.CONNECTED:
            raise RuntimeError(f"Cannot send in state {self._state}")

        if self._native_ws:
            if hasattr(self._native_ws, "send_bytes"):
                if asyncio.iscoroutinefunction(self._native_ws.send_bytes):
                    await self._native_ws.send_bytes(data)
                else:
                    self._native_ws.send_bytes(data)
            elif hasattr(self._native_ws, "send"):
                if asyncio.iscoroutinefunction(self._native_ws.send):
                    await self._native_ws.send(data)
                else:
                    self._native_ws.send(data)
        elif self._send:
            await self._send(
                {
                    "type": "websocket.send",
                    "bytes": data,
                }
            )

    async def send_json(
        self,
        data: Any,
        mode: str = "text",
    ) -> None:
        """
        Send a JSON message.

        Args:
            data: Data to serialize as JSON
            mode: "text" for text frame, "binary" for binary frame
        """
        encoded = json.dumps(data, ensure_ascii=False)
        if mode == "text":
            await self.send_text(encoded)
        else:
            await self.send_bytes(encoded.encode("utf-8"))

    async def receive(self) -> Dict[str, Any]:
        """
        Receive a raw WebSocket message.

        Returns a dict with 'type', and optionally 'text' or 'bytes'.
        """
        if self._state == WebSocketState.DISCONNECTED:
            raise WebSocketDisconnect()

        if self._native_ws:
            try:
                if hasattr(self._native_ws, "receive"):
                    if asyncio.iscoroutinefunction(self._native_ws.receive):
                        data = await self._native_ws.receive()
                    else:
                        data = self._native_ws.receive()

                    # Normalize to ASGI format
                    if isinstance(data, str):
                        return {"type": "websocket.receive", "text": data}
                    elif isinstance(data, bytes):
                        return {"type": "websocket.receive", "bytes": data}
                    elif isinstance(data, dict):
                        return data
                    else:
                        return {"type": "websocket.receive", "text": str(data)}
            except Exception as e:
                self._state = WebSocketState.DISCONNECTED
                if "close" in str(e).lower() or "disconnect" in str(e).lower():
                    raise WebSocketDisconnect()
                raise
        elif self._receive:
            message = await self._receive()

            if message["type"] == "websocket.disconnect":
                self._state = WebSocketState.DISCONNECTED
                code = message.get("code", 1000)
                raise WebSocketDisconnect(code=code)

            return message

        raise RuntimeError("No receive method available")

    async def receive_text(self) -> str:
        """Receive a text message."""
        message = await self.receive()

        if message["type"] == "websocket.disconnect":
            raise WebSocketDisconnect(code=message.get("code", 1000))

        if "text" in message:
            return message["text"]
        elif "bytes" in message:
            return message["bytes"].decode("utf-8")

        raise RuntimeError("Expected text message")

    async def receive_bytes(self) -> bytes:
        """Receive a binary message."""
        message = await self.receive()

        if message["type"] == "websocket.disconnect":
            raise WebSocketDisconnect(code=message.get("code", 1000))

        if "bytes" in message:
            return message["bytes"]
        elif "text" in message:
            return message["text"].encode("utf-8")

        raise RuntimeError("Expected binary message")

    async def receive_json(self, mode: str = "text") -> Any:
        """
        Receive and parse a JSON message.

        Args:
            mode: "text" to expect text frame, "binary" for binary frame
        """
        if mode == "text":
            data = await self.receive_text()
        else:
            data = await self.receive_bytes()
            data = data.decode("utf-8")

        return json.loads(data)

    async def iter_text(self):
        """Iterate over text messages until disconnect."""
        try:
            while True:
                yield await self.receive_text()
        except WebSocketDisconnect:
            return

    async def iter_bytes(self):
        """Iterate over binary messages until disconnect."""
        try:
            while True:
                yield await self.receive_bytes()
        except WebSocketDisconnect:
            return

    async def iter_json(self):
        """Iterate over JSON messages until disconnect."""
        try:
            while True:
                yield await self.receive_json()
        except WebSocketDisconnect:
            return


# WebSocket close codes (RFC 6455)
class WebSocketClose:
    """Standard WebSocket close codes."""

    NORMAL_CLOSURE = 1000
    GOING_AWAY = 1001
    PROTOCOL_ERROR = 1002
    UNSUPPORTED_DATA = 1003
    NO_STATUS_RECEIVED = 1005
    ABNORMAL_CLOSURE = 1006
    INVALID_FRAME_PAYLOAD_DATA = 1007
    POLICY_VIOLATION = 1008
    MESSAGE_TOO_BIG = 1009
    MANDATORY_EXTENSION = 1010
    INTERNAL_ERROR = 1011
    SERVICE_RESTART = 1012
    TRY_AGAIN_LATER = 1013
    BAD_GATEWAY = 1014
    TLS_HANDSHAKE = 1015


__all__ = [
    "WebSocket",
    "WebSocketState",
    "WebSocketDisconnect",
    "WebSocketClose",
]
