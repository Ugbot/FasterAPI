#!/usr/bin/env python3
"""
FasterAPI Full Integration Example

Demonstrates the complete unified framework with:
- HTTP/1.1, HTTP/2, HTTP/3 support
- WebSocket support
- zstd compression
- PostgreSQL integration
- FastAPI-compatible API
- Middleware and lifecycle hooks
"""

from fasterapi import App, Depends
from fasterapi.pg import PgPool
from fasterapi.http import Request, Response
import time
import json

# Create PostgreSQL connection pool
pool = PgPool(
    "postgres://localhost/fasterapi_test",
    2,  # min_connections
    10  # max_connections
)

# Create FasterAPI application
app = App(
    port=8000,
    host="0.0.0.0",
    enable_h2=True,      # Enable HTTP/2
    enable_h3=False,     # Disable HTTP/3 for now
    enable_compression=True  # Enable zstd compression
)

# Middleware for request logging
@app.add_middleware
def logging_middleware(req: Request, res: Response):
    start_time = time.time()
    print(f"📥 {req.get_method().value} {req.get_path()} - {req.get_client_ip()}")
    
    # Add timing header
    res.header("X-Process-Time", f"{time.time() - start_time:.3f}s")

# Lifecycle hooks
@app.on_event("startup")
def startup():
    print("🚀 FasterAPI v0.1.0")
    print("Starting server on http://0.0.0.0:8000")
    print("Features: HTTP/2=True, HTTP/3=False")
    print(f"Routes: {len(app.routes)}")

@app.on_event("shutdown")
def shutdown():
    print("🛑 Shutting down FasterAPI...")
    pool.close()

# Health check endpoint
@app.get("/health")
def health_check(req: Request, res: Response):
    """Health check endpoint."""
    return {
        "status": "healthy",
        "timestamp": time.time(),
        "version": "0.1.0",
        "features": {
            "http2": app.enable_h2,
            "http3": app.enable_h3,
            "compression": app.enable_compression
        }
    }

# Simple ping endpoint
@app.get("/ping")
def ping(req: Request, res: Response):
    """Simple ping endpoint."""
    return {"message": "pong", "timestamp": time.time()}

# Echo endpoint with request data
@app.post("/echo")
def echo(req: Request, res: Response):
    """Echo back request data."""
    try:
        if req.is_json():
            data = req.json()
        else:
            data = {"body": req.get_body()}
        
        return {
            "echo": data,
            "headers": dict(req.get_headers()),
            "method": req.get_method().value,
            "path": req.get_path(),
            "protocol": req.get_protocol()
        }
    except Exception as e:
        res.status(400).json({"error": str(e)})

# PostgreSQL integration example
@app.get("/items/{item_id}")
def get_item(item_id: int, pg = Depends(lambda: pool.get())):
    """Get item by ID from PostgreSQL."""
    try:
        # Execute query
        result = pg.exec("SELECT * FROM items WHERE id = $1", item_id)
        
        if result.row_count() == 0:
            return {"error": "Item not found"}, 404
        
        # Convert row to dictionary
        row = result.one()
        return dict(row)
        
    except Exception as e:
        return {"error": str(e)}, 500
    finally:
        # Connection will be released automatically by Depends
        pass

# Create item endpoint
@app.post("/items")
def create_item(req: Request, res: Response, pg = Depends(lambda: pool.get())):
    """Create a new item."""
    try:
        data = req.json()
        
        # Insert new item
        result = pg.exec(
            "INSERT INTO items (name, description, price) VALUES ($1, $2, $3) RETURNING id",
            data.get("name"),
            data.get("description"),
            data.get("price", 0.0)
        )
        
        new_id = result.scalar()
        return {"id": new_id, "message": "Item created successfully"}
        
    except Exception as e:
        return {"error": str(e)}, 500

# Bulk operations with COPY
@app.post("/items/bulk")
def bulk_create_items(req: Request, res: Response, pg = Depends(lambda: pool.get())):
    """Create multiple items using COPY for performance."""
    try:
        data = req.json()
        items = data.get("items", [])
        
        if not items:
            return {"error": "No items provided"}, 400
        
        # Start COPY operation
        pg.copy_in("COPY items (name, description, price) FROM stdin CSV")
        
        # Write data
        for item in items:
            line = f"{item.get('name')},{item.get('description')},{item.get('price', 0.0)}\n"
            pg.copy_in_write(line.encode())
        
        # End COPY operation
        pg.copy_in_end()
        
        return {"message": f"Created {len(items)} items successfully"}
        
    except Exception as e:
        return {"error": str(e)}, 500

# WebSocket chat example
@app.websocket("/ws/chat")
def chat_websocket(ws):
    """WebSocket chat endpoint."""
    print("🔌 WebSocket connection opened")
    
    # Send welcome message
    ws.send_text(json.dumps({
        "type": "welcome",
        "message": "Welcome to FasterAPI WebSocket chat!"
    }))
    
    # Handle incoming messages
    def on_message(message, opcode):
        try:
            data = json.loads(message)
            print(f"📨 Received: {data}")
            
            # Echo back with timestamp
            response = {
                "type": "echo",
                "original": data,
                "timestamp": time.time()
            }
            ws.send_text(json.dumps(response))
            
        except json.JSONDecodeError:
            ws.send_text(json.dumps({
                "type": "error",
                "message": "Invalid JSON"
            }))
    
    # Handle connection close
    def on_close(code, reason):
        print(f"🔌 WebSocket connection closed: {code} - {reason}")
    
    # Set callbacks
    ws.set_callbacks(
        on_message=on_message,
        on_close=on_close
    )

# Performance test endpoint
@app.get("/perf/test")
def performance_test(req: Request, res: Response):
    """Performance test endpoint."""
    start_time = time.time()
    
    # Simulate some work
    data = []
    for i in range(1000):
        data.append({
            "id": i,
            "value": f"item_{i}",
            "timestamp": time.time()
        })
    
    processing_time = time.time() - start_time
    
    return {
        "message": "Performance test completed",
        "items_count": len(data),
        "processing_time": processing_time,
        "items_per_second": len(data) / processing_time if processing_time > 0 else 0
    }

# Error handling example
@app.get("/error")
def error_example(req: Request, res: Response):
    """Example endpoint that raises an error."""
    raise ValueError("This is a test error")

# File serving example
@app.get("/files/{filename}")
def serve_file(filename: str, req: Request, res: Response):
    """Serve static files."""
    try:
        # In a real application, you'd serve from a static directory
        content = f"Content of {filename}\nGenerated at {time.time()}"
        return res.text(content)
    except Exception as e:
        return {"error": str(e)}, 500

if __name__ == "__main__":
    print("🚀 Starting FasterAPI Full Integration Example")
    print("=" * 50)
    
    # Run the application
    try:
        app.run()
    except KeyboardInterrupt:
        print("\n👋 Goodbye!")
    except Exception as e:
        print(f"❌ Error: {e}")
        exit(1)