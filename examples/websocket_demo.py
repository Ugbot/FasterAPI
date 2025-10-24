"""
WebSocket Demo

Demonstrates WebSocket support in FasterAPI.
Features:
- Echo server
- Text and binary messages
- Ping/pong
- Connection management
"""

import asyncio
from fasterapi import App, WebSocket

app = App(port=8000)


@app.websocket("/ws")
async def websocket_echo(ws: WebSocket):
    """
    Simple WebSocket echo server.
    
    Echoes back any text or binary message received.
    """
    print(f"WebSocket connection opened: {ws}")
    
    try:
        # Send welcome message
        await ws.send_text("Welcome to FasterAPI WebSocket!")
        
        # Echo loop
        while ws.is_open():
            message = await ws.receive()
            
            if isinstance(message, str):
                print(f"Received text: {message}")
                await ws.send_text(f"Echo: {message}")
            elif isinstance(message, bytes):
                print(f"Received binary: {len(message)} bytes")
                await ws.send_binary(message)
    
    except Exception as e:
        print(f"WebSocket error: {e}")
    
    finally:
        print(f"WebSocket connection closed: {ws}")


@app.websocket("/ws/ping")
async def websocket_ping(ws: WebSocket):
    """
    WebSocket ping server.
    
    Sends ping every 5 seconds and waits for messages.
    """
    print("Ping WebSocket opened")
    
    try:
        while ws.is_open():
            # Send ping
            ws.ping()
            print("Sent ping")
            
            # Wait for message or timeout
            try:
                message = await asyncio.wait_for(ws.receive(), timeout=5.0)
                await ws.send_text(f"Got: {message}")
            except asyncio.TimeoutError:
                # No message, continue
                pass
    
    except Exception as e:
        print(f"Error: {e}")


@app.websocket("/ws/counter")
async def websocket_counter(ws: WebSocket):
    """
    WebSocket counter.
    
    Sends incrementing counter every second.
    """
    print("Counter WebSocket opened")
    
    try:
        counter = 0
        
        while ws.is_open():
            await ws.send_text(f"Count: {counter}")
            counter += 1
            await asyncio.sleep(1)
    
    except Exception as e:
        print(f"Error: {e}")


@app.get("/")
def home(req, res):
    """
    Serve simple HTML client for testing.
    """
    html = """
    <!DOCTYPE html>
    <html>
    <head>
        <title>FasterAPI WebSocket Demo</title>
        <style>
            body { font-family: Arial, sans-serif; margin: 40px; }
            #messages { 
                border: 1px solid #ccc; 
                height: 300px; 
                overflow-y: scroll; 
                padding: 10px;
                margin: 10px 0;
            }
            input, button { padding: 10px; margin: 5px 0; }
            input { width: 400px; }
            .message { margin: 5px 0; }
            .sent { color: blue; }
            .received { color: green; }
            .system { color: gray; font-style: italic; }
        </style>
    </head>
    <body>
        <h1>FasterAPI WebSocket Demo</h1>
        
        <div>
            <button onclick="connect()">Connect</button>
            <button onclick="disconnect()">Disconnect</button>
            <span id="status">Disconnected</span>
        </div>
        
        <div id="messages"></div>
        
        <div>
            <input type="text" id="messageInput" placeholder="Type a message..." onkeypress="if(event.key==='Enter')sendMessage()">
            <button onclick="sendMessage()">Send</button>
            <button onclick="sendPing()">Ping</button>
        </div>
        
        <script>
            let ws = null;
            
            function connect() {
                ws = new WebSocket('ws://localhost:8000/ws');
                
                ws.onopen = () => {
                    addMessage('Connected', 'system');
                    document.getElementById('status').textContent = 'Connected';
                };
                
                ws.onmessage = (event) => {
                    addMessage(event.data, 'received');
                };
                
                ws.onclose = () => {
                    addMessage('Disconnected', 'system');
                    document.getElementById('status').textContent = 'Disconnected';
                };
                
                ws.onerror = (error) => {
                    addMessage('Error: ' + error, 'system');
                };
            }
            
            function disconnect() {
                if (ws) {
                    ws.close();
                    ws = null;
                }
            }
            
            function sendMessage() {
                const input = document.getElementById('messageInput');
                const message = input.value;
                
                if (ws && ws.readyState === WebSocket.OPEN && message) {
                    ws.send(message);
                    addMessage(message, 'sent');
                    input.value = '';
                }
            }
            
            function sendPing() {
                if (ws && ws.readyState === WebSocket.OPEN) {
                    ws.send('PING');
                    addMessage('PING', 'sent');
                }
            }
            
            function addMessage(text, type) {
                const messages = document.getElementById('messages');
                const div = document.createElement('div');
                div.className = 'message ' + type;
                div.textContent = text;
                messages.appendChild(div);
                messages.scrollTop = messages.scrollHeight;
            }
        </script>
    </body>
    </html>
    """
    
    res.set_header("Content-Type", "text/html")
    return html


def main():
    print("╔══════════════════════════════════════════╗")
    print("║     FasterAPI WebSocket Demo            ║")
    print("╚══════════════════════════════════════════╝")
    print()
    print("Server starting on http://localhost:8000")
    print()
    print("WebSocket endpoints:")
    print("  ws://localhost:8000/ws         - Echo server")
    print("  ws://localhost:8000/ws/ping    - Ping server")
    print("  ws://localhost:8000/ws/counter - Counter")
    print()
    print("Open http://localhost:8000 in your browser to test")
    print()
    
    app.run()


if __name__ == "__main__":
    main()





