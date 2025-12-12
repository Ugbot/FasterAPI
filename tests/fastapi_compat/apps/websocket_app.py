"""
WebSocket application for FastAPI compatibility testing.

Tests:
- WebSocket connection
- Text message send/receive
- JSON message send/receive
- Binary message send/receive
- WebSocket disconnect handling
- Path parameters in WebSocket
- Query parameters in WebSocket

Can be run with either FastAPI or FasterAPI by setting TEST_FRAMEWORK env var.
"""

import json
import os
from typing import Dict, List, Set
from uuid import uuid4

# Import framework based on environment
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi import FastAPI, Query, WebSocket, WebSocketDisconnect
else:
    from fasterapi import FastAPI, Query, WebSocket, WebSocketDisconnect


# Track connections for testing
active_connections: Dict[str, WebSocket] = {}
message_log: List[Dict] = []
connection_events: List[Dict] = []


class ConnectionManager:
    """Manages WebSocket connections."""

    def __init__(self):
        self.connections: Dict[str, WebSocket] = {}
        self.rooms: Dict[str, Set[str]] = {}

    async def connect(self, websocket: WebSocket, client_id: str):
        """Accept and register a new connection."""
        await websocket.accept()
        self.connections[client_id] = websocket
        connection_events.append(
            {
                "event": "connect",
                "client_id": client_id,
            }
        )

    def disconnect(self, client_id: str):
        """Remove a connection."""
        if client_id in self.connections:
            del self.connections[client_id]
        # Remove from all rooms
        for room in self.rooms.values():
            room.discard(client_id)
        connection_events.append(
            {
                "event": "disconnect",
                "client_id": client_id,
            }
        )

    async def send_personal(self, client_id: str, message: str):
        """Send message to a specific client."""
        if client_id in self.connections:
            await self.connections[client_id].send_text(message)

    async def broadcast(self, message: str, exclude: str = None):
        """Broadcast message to all connected clients."""
        for client_id, websocket in self.connections.items():
            if client_id != exclude:
                await websocket.send_text(message)

    def join_room(self, client_id: str, room: str):
        """Add client to a room."""
        if room not in self.rooms:
            self.rooms[room] = set()
        self.rooms[room].add(client_id)

    def leave_room(self, client_id: str, room: str):
        """Remove client from a room."""
        if room in self.rooms:
            self.rooms[room].discard(client_id)

    async def broadcast_to_room(self, room: str, message: str, exclude: str = None):
        """Broadcast message to all clients in a room."""
        if room in self.rooms:
            for client_id in self.rooms[room]:
                if client_id != exclude and client_id in self.connections:
                    await self.connections[client_id].send_text(message)


manager = ConnectionManager()


def create_app() -> FastAPI:
    """Create and configure the FastAPI application."""

    app = FastAPI(
        title="WebSocket Test App",
        description="WebSocket testing application",
        version="1.0.0",
    )

    @app.get("/health")
    async def health():
        """Health check."""
        return {
            "status": "healthy",
            "framework": FRAMEWORK,
            "active_connections": len(manager.connections),
        }

    @app.get("/connections")
    async def get_connections():
        """Get list of active connections."""
        return {
            "connections": list(manager.connections.keys()),
            "count": len(manager.connections),
        }

    @app.get("/connection-events")
    async def get_connection_events():
        """Get connection event log."""
        return {"events": connection_events}

    @app.get("/message-log")
    async def get_message_log():
        """Get message log."""
        return {"messages": message_log}

    @app.post("/clear-logs")
    async def clear_logs():
        """Clear all logs."""
        message_log.clear()
        connection_events.clear()
        return {"message": "Logs cleared"}

    # Basic echo WebSocket
    @app.websocket("/ws/echo")
    async def websocket_echo(websocket: WebSocket):
        """Simple echo WebSocket - returns whatever is sent."""
        await websocket.accept()
        try:
            while True:
                data = await websocket.receive_text()
                message_log.append({"endpoint": "echo", "received": data})
                await websocket.send_text(f"Echo: {data}")
        except WebSocketDisconnect:
            pass

    # WebSocket with client ID path parameter
    @app.websocket("/ws/{client_id}")
    async def websocket_with_client_id(websocket: WebSocket, client_id: str):
        """WebSocket with client ID in path."""
        await manager.connect(websocket, client_id)
        try:
            await websocket.send_text(f"Welcome {client_id}!")
            while True:
                data = await websocket.receive_text()
                message_log.append(
                    {
                        "endpoint": "client_id",
                        "client_id": client_id,
                        "received": data,
                    }
                )
                # Broadcast to all other clients
                await manager.broadcast(f"{client_id}: {data}", exclude=client_id)
                # Confirm to sender
                await websocket.send_text(f"Sent: {data}")
        except WebSocketDisconnect:
            manager.disconnect(client_id)

    # JSON WebSocket
    @app.websocket("/ws/json")
    async def websocket_json(websocket: WebSocket):
        """WebSocket that handles JSON messages."""
        await websocket.accept()
        try:
            while True:
                data = await websocket.receive_json()
                message_log.append({"endpoint": "json", "received": data})

                # Process based on action
                action = data.get("action", "echo")

                if action == "echo":
                    await websocket.send_json({"action": "echo", "data": data})
                elif action == "ping":
                    await websocket.send_json(
                        {"action": "pong", "timestamp": data.get("timestamp")}
                    )
                elif action == "reverse":
                    text = data.get("text", "")
                    await websocket.send_json(
                        {"action": "reversed", "text": text[::-1]}
                    )
                else:
                    await websocket.send_json({"action": "unknown", "original": action})
        except WebSocketDisconnect:
            pass

    # Binary WebSocket
    @app.websocket("/ws/binary")
    async def websocket_binary(websocket: WebSocket):
        """WebSocket that handles binary messages."""
        await websocket.accept()
        try:
            while True:
                data = await websocket.receive_bytes()
                message_log.append(
                    {
                        "endpoint": "binary",
                        "received_size": len(data),
                    }
                )
                # Echo back with length prefix
                response = len(data).to_bytes(4, "big") + data
                await websocket.send_bytes(response)
        except WebSocketDisconnect:
            pass

    # Chat room WebSocket
    @app.websocket("/ws/chat/{room_id}")
    async def websocket_chat_room(
        websocket: WebSocket,
        room_id: str,
        username: str = Query(default="anonymous"),
    ):
        """WebSocket chat room with room ID and username."""
        client_id = f"{room_id}_{username}_{uuid4().hex[:8]}"
        await manager.connect(websocket, client_id)
        manager.join_room(client_id, room_id)

        try:
            # Announce join
            join_msg = json.dumps(
                {
                    "type": "system",
                    "message": f"{username} joined the room",
                }
            )
            await manager.broadcast_to_room(room_id, join_msg)

            while True:
                data = await websocket.receive_text()

                message_log.append(
                    {
                        "endpoint": "chat",
                        "room": room_id,
                        "username": username,
                        "received": data,
                    }
                )

                # Broadcast to room
                chat_msg = json.dumps(
                    {
                        "type": "message",
                        "username": username,
                        "message": data,
                    }
                )
                await manager.broadcast_to_room(room_id, chat_msg)
        except WebSocketDisconnect:
            manager.disconnect(client_id)
            # Announce leave
            leave_msg = json.dumps(
                {
                    "type": "system",
                    "message": f"{username} left the room",
                }
            )
            await manager.broadcast_to_room(room_id, leave_msg)

    # Counter WebSocket (stateful)
    @app.websocket("/ws/counter")
    async def websocket_counter(websocket: WebSocket):
        """WebSocket with server-side state (counter)."""
        await websocket.accept()
        counter = 0

        try:
            while True:
                data = await websocket.receive_text()

                if data == "increment":
                    counter += 1
                elif data == "decrement":
                    counter -= 1
                elif data == "reset":
                    counter = 0
                elif data == "get":
                    pass  # Just return current value
                else:
                    try:
                        counter = int(data)
                    except ValueError:
                        await websocket.send_json({"error": "Invalid command"})
                        continue

                await websocket.send_json({"counter": counter})
        except WebSocketDisconnect:
            pass

    # Iteration test WebSocket
    @app.websocket("/ws/iterate")
    async def websocket_iterate(websocket: WebSocket):
        """WebSocket for testing iter_text/iter_json."""
        await websocket.accept()

        try:
            # Use iterator pattern
            async for message in websocket.iter_text():
                message_log.append({"endpoint": "iterate", "received": message})
                await websocket.send_text(f"Received: {message}")
        except WebSocketDisconnect:
            pass

    return app


# Create app instance
app = create_app()


def clear_all():
    """Clear all state (for testing)."""
    manager.connections.clear()
    manager.rooms.clear()
    message_log.clear()
    connection_events.clear()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
