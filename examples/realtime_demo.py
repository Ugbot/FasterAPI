"""
Real-Time Communication Demo

Demonstrates both Server-Sent Events (SSE) and WebSockets.

SSE: Server -> Client (one-way)
- Simpler protocol
- Auto-reconnection
- Event types & IDs
- HTTP-based

WebSocket: Bidirectional
- Full duplex
- Custom protocol
- Lower overhead
- Persistent connection
"""

import asyncio
import time
import json
from fasterapi import App
from fasterapi.http import Request, Response
from fasterapi.http.sse import SSEConnection, sse_endpoint
from fasterapi.http import WebSocket


app = App(port=8000)


# ============================================================================
# Server-Sent Events (SSE) Examples
# ============================================================================

@app.get("/sse/time")
def sse_time_stream(req: Request, res: Response):
    """
    SSE Example 1: Time updates every second.
    
    Client code:
        const es = new EventSource('/sse/time');
        es.addEventListener('time', (e) => {
            console.log('Time:', JSON.parse(e.data));
        });
    """
    def handle_sse(sse: SSEConnection):
        print(f"SSE client connected: {sse.get_id()}")
        
        try:
            for i in range(10):  # Send 10 updates
                if not sse.is_open():
                    break
                
                sse.send(
                    {"timestamp": time.time(), "count": i},
                    event="time",
                    id=str(i)
                )
                time.sleep(1)
                
                # Send keep-alive every few seconds
                if i % 3 == 0:
                    sse.ping()
        
        finally:
            print(f"SSE client disconnected: {sse.get_id()}")
            sse.close()
    
    from fasterapi.http.sse import SSEResponse
    return SSEResponse.create(handle_sse)


@app.get("/sse/progress")
def sse_progress_stream(req: Request, res: Response):
    """
    SSE Example 2: Progress bar updates.
    
    Simulates a long-running task with progress updates.
    
    Client code:
        const es = new EventSource('/sse/progress');
        es.addEventListener('progress', (e) => {
            const data = JSON.parse(e.data);
            updateProgressBar(data.percent);
        });
    """
    def handle_sse(sse: SSEConnection):
        print("Progress stream started")
        
        # Simulate task with 10 steps
        for step in range(11):
            if not sse.is_open():
                break
            
            percent = step * 10
            sse.send(
                {
                    "percent": percent,
                    "step": step,
                    "total_steps": 10,
                    "message": f"Step {step}/10"
                },
                event="progress",
                id=f"step{step}"
            )
            
            time.sleep(0.5)  # Simulate work
        
        # Final completion event
        sse.send(
            {"percent": 100, "status": "complete"},
            event="done"
        )
        
        sse.close()
    
    from fasterapi.http.sse import SSEResponse
    return SSEResponse.create(handle_sse)


@app.get("/sse/chat")
def sse_chat_stream(req: Request, res: Response):
    """
    SSE Example 3: Chat message stream.
    
    Broadcasts chat messages to connected clients.
    
    Client code:
        const es = new EventSource('/sse/chat');
        es.addEventListener('message', (e) => {
            const msg = JSON.parse(e.data);
            displayMessage(msg.user, msg.text);
        });
    """
    def handle_sse(sse: SSEConnection):
        print("Chat client connected")
        
        # Simulate chat messages
        messages = [
            {"user": "Alice", "text": "Hello everyone!"},
            {"user": "Bob", "text": "Hi Alice!"},
            {"user": "Charlie", "text": "Hey folks!"},
            {"user": "Alice", "text": "How's everyone doing?"},
            {"user": "Bob", "text": "Great! Working on FasterAPI."},
        ]
        
        for i, msg in enumerate(messages):
            if not sse.is_open():
                break
            
            sse.send(msg, event="message", id=f"msg{i}")
            time.sleep(1)
        
        sse.close()
    
    from fasterapi.http.sse import SSEResponse
    return SSEResponse.create(handle_sse)


@app.get("/sse/metrics")
def sse_metrics_stream(req: Request, res: Response):
    """
    SSE Example 4: Live system metrics.
    
    Streams real-time system metrics.
    
    Client code:
        const es = new EventSource('/sse/metrics');
        es.addEventListener('metrics', (e) => {
            const data = JSON.parse(e.data);
            updateDashboard(data);
        });
    """
    def handle_sse(sse: SSEConnection):
        print("Metrics stream started")
        
        import random
        
        for i in range(20):  # 20 updates
            if not sse.is_open():
                break
            
            # Simulate metrics
            metrics = {
                "cpu": 40 + random.randint(-10, 20),
                "memory": 60 + random.randint(-5, 10),
                "requests_per_sec": 1000 + random.randint(-100, 200),
                "active_connections": 50 + random.randint(-10, 20),
                "timestamp": time.time()
            }
            
            sse.send(metrics, event="metrics", id=f"m{i}")
            time.sleep(0.5)
        
        sse.close()
    
    from fasterapi.http.sse import SSEResponse
    return SSEResponse.create(handle_sse)


# ============================================================================
# WebSocket Examples (for comparison)
# ============================================================================

@app.websocket("/ws/echo")
def ws_echo(ws: WebSocket):
    """
    WebSocket Example 1: Echo server.
    
    Bidirectional communication.
    
    Client code:
        const ws = new WebSocket('ws://localhost:8000/ws/echo');
        ws.onmessage = (e) => console.log('Received:', e.data);
        ws.send('Hello Server');
    """
    print(f"WebSocket client connected")
    
    # In real implementation:
    # while ws.is_connected():
    #     message = ws.receive()
    #     ws.send(f"Echo: {message}")
    
    print("WebSocket client disconnected")


@app.websocket("/ws/chat")
def ws_chat(ws: WebSocket):
    """
    WebSocket Example 2: Chat room.
    
    Real-time bidirectional chat.
    
    Client code:
        const ws = new WebSocket('ws://localhost:8000/ws/chat');
        ws.onmessage = (e) => displayMessage(JSON.parse(e.data));
        ws.send(JSON.stringify({user: 'Alice', text: 'Hello!'}));
    """
    print("Chat WebSocket connected")
    
    # In real implementation:
    # while ws.is_connected():
    #     msg = json.loads(ws.receive())
    #     broadcast_to_all(msg)  # Send to all connected clients
    
    print("Chat WebSocket disconnected")


# ============================================================================
# Comparison Endpoints
# ============================================================================

@app.get("/")
def index(req: Request, res: Response):
    """Index with links to all demos."""
    return {
        "title": "FasterAPI Real-Time Communication Demo",
        "sse_endpoints": {
            "/sse/time": "Time updates every second",
            "/sse/progress": "Progress bar simulation",
            "/sse/chat": "Chat message stream",
            "/sse/metrics": "Live system metrics"
        },
        "websocket_endpoints": {
            "/ws/echo": "Echo server (bidirectional)",
            "/ws/chat": "Chat room (bidirectional)"
        },
        "comparison": {
            "sse": {
                "direction": "server -> client only",
                "protocol": "HTTP",
                "reconnection": "automatic",
                "use_cases": ["notifications", "live updates", "metrics"]
            },
            "websocket": {
                "direction": "bidirectional",
                "protocol": "WebSocket",
                "reconnection": "manual",
                "use_cases": ["chat", "gaming", "collaboration"]
            }
        }
    }


@app.get("/demo")
def demo_page(req: Request, res: Response):
    """
    HTML demo page showing SSE and WebSocket usage.
    """
    html = """
<!DOCTYPE html>
<html>
<head>
    <title>FasterAPI Real-Time Demo</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 1200px; margin: 20px auto; padding: 20px; }
        .section { margin: 20px 0; padding: 20px; border: 1px solid #ddd; border-radius: 8px; }
        h1 { color: #333; }
        h2 { color: #666; }
        .log { background: #f5f5f5; padding: 10px; border-radius: 4px; max-height: 200px; overflow-y: auto; font-family: monospace; }
        button { padding: 10px 20px; margin: 5px; cursor: pointer; }
    </style>
</head>
<body>
    <h1>🚀 FasterAPI Real-Time Communication Demo</h1>
    
    <div class="section">
        <h2>📡 Server-Sent Events (SSE)</h2>
        <button onclick="connectSSETime()">Connect to Time Stream</button>
        <button onclick="connectSSEProgress()">Start Progress Bar</button>
        <button onclick="connectSSEMetrics()">Live Metrics</button>
        <div class="log" id="sse-log"></div>
    </div>
    
    <div class="section">
        <h2>🔌 WebSocket</h2>
        <button onclick="connectWSEcho()">Connect Echo</button>
        <button onclick="sendWSMessage()">Send Message</button>
        <div class="log" id="ws-log"></div>
    </div>
    
    <script>
        let sseLog = document.getElementById('sse-log');
        let wsLog = document.getElementById('ws-log');
        let ws = null;
        
        function log(element, message) {
            element.innerHTML += message + '<br>';
            element.scrollTop = element.scrollHeight;
        }
        
        function connectSSETime() {
            const es = new EventSource('/sse/time');
            es.addEventListener('time', (e) => {
                const data = JSON.parse(e.data);
                log(sseLog, `⏰ Time update ${data.count}: ${new Date(data.timestamp * 1000).toLocaleTimeString()}`);
            });
            log(sseLog, '✅ Connected to time stream');
        }
        
        function connectSSEProgress() {
            const es = new EventSource('/sse/progress');
            es.addEventListener('progress', (e) => {
                const data = JSON.parse(e.data);
                log(sseLog, `📊 Progress: ${data.percent}% - ${data.message}`);
            });
            es.addEventListener('done', (e) => {
                log(sseLog, '✅ Task complete!');
                es.close();
            });
            log(sseLog, '✅ Progress stream started');
        }
        
        function connectSSEMetrics() {
            const es = new EventSource('/sse/metrics');
            es.addEventListener('metrics', (e) => {
                const data = JSON.parse(e.data);
                log(sseLog, `📈 CPU: ${data.cpu}% | Memory: ${data.memory}% | RPS: ${data.requests_per_sec}`);
            });
            log(sseLog, '✅ Metrics stream started');
        }
        
        function connectWSEcho() {
            ws = new WebSocket('ws://localhost:8000/ws/echo');
            ws.onopen = () => log(wsLog, '✅ WebSocket connected');
            ws.onmessage = (e) => log(wsLog, `📩 Received: ${e.data}`);
            ws.onclose = () => log(wsLog, '❌ WebSocket closed');
        }
        
        function sendWSMessage() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                const msg = 'Hello at ' + new Date().toLocaleTimeString();
                ws.send(msg);
                log(wsLog, `📤 Sent: ${msg}`);
            } else {
                log(wsLog, '❌ Not connected. Click "Connect Echo" first.');
            }
        }
    </script>
</body>
</html>
    """
    
    res.content_type("text/html").text(html).send()


# ============================================================================
# Lifecycle Hooks
# ============================================================================

@app.on_event("startup")
def startup():
    """Application startup."""
    print()
    print("╔══════════════════════════════════════════════════════════╗")
    print("║     FasterAPI Real-Time Communication Demo              ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    print("🚀 Server starting on http://localhost:8000")
    print()
    print("🌐 Open http://localhost:8000/demo in your browser")
    print()
    print("📡 SSE Endpoints:")
    print("   /sse/time - Time updates every second")
    print("   /sse/progress - Progress bar simulation")
    print("   /sse/chat - Chat message stream")
    print("   /sse/metrics - Live system metrics")
    print()
    print("🔌 WebSocket Endpoints:")
    print("   /ws/echo - Echo server")
    print("   /ws/chat - Chat room")
    print()
    print("ℹ️  SSE vs WebSocket:")
    print("   SSE: One-way (server→client), HTTP-based, auto-reconnect")
    print("   WS:  Bidirectional, custom protocol, full-duplex")
    print()


def main():
    """Run the demo."""
    app.run()


if __name__ == "__main__":
    main()

