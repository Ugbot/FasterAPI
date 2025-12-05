"""
Test handlers module for sub-interpreter benchmarking.
Handlers in this module can be imported by sub-interpreters.
"""
import time

def root():
    """Basic endpoint handler."""
    return {"message": "OK", "timestamp": time.time()}

def compute():
    """CPU-bound endpoint to test parallelism."""
    total = 0
    for i in range(10000):
        total += i * i
    return {"result": total}

def get_user(user_id: int):
    """Handler with path parameter."""
    return {"id": user_id, "name": f"User {user_id}"}


# =============================================================================
# WebSocket Handlers
# =============================================================================

async def ws_echo_handler(websocket):
    """Echo WebSocket handler - echoes back any message received."""
    print(f"[WS] Connection opened: {websocket.path}")
    try:
        while True:
            message = await websocket.receive()
            if message is None:
                print(f"[WS] Connection closed by client")
                break
            print(f"[WS] Received: {message}")
            # Echo the message back
            if isinstance(message, bytes):
                await websocket.send_binary(message)
            else:
                await websocket.send_text(f"Echo: {message}")
    except Exception as e:
        print(f"[WS] Error: {e}")
    finally:
        print(f"[WS] Handler done")


async def ws_json_handler(websocket):
    """JSON WebSocket handler - parses JSON and responds with JSON."""
    import json
    print(f"[WS JSON] Connection opened: {websocket.path}")
    try:
        while True:
            message = await websocket.receive()
            if message is None:
                break
            try:
                data = json.loads(message)
                response = {"received": data, "status": "ok"}
                await websocket.send_text(json.dumps(response))
            except json.JSONDecodeError:
                await websocket.send_text(json.dumps({"error": "Invalid JSON"}))
    except Exception as e:
        print(f"[WS JSON] Error: {e}")
    finally:
        print(f"[WS JSON] Handler done")
