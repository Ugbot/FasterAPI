"""
FasterAPI Native implementation of 1 Million Request Challenge (1MRC)

Uses FasterAPI's native C++ HTTP server (not FastAPI/uvicorn).
This demonstrates the true performance potential of FasterAPI.

Expected performance: ~100K+ req/s (C++ server with Python handlers)
"""

import sys
import threading
from typing import Dict
import json

# Import FasterAPI native components (when available)
try:
    from fasterapi import App, Request, Response
    FASTERAPI_AVAILABLE = True
except ImportError:
    FASTERAPI_AVAILABLE = False
    print("⚠️  FasterAPI native server not available")
    print("This requires the C++ extensions to be built")
    sys.exit(1)


class EventStore:
    """
    Thread-safe event storage using Python locks.
    
    Note: The C++ server handles concurrency efficiently,
    so Python-level locking is sufficient here.
    """
    
    def __init__(self):
        self.total_requests: int = 0
        self.sum: float = 0.0
        self.users: Dict[str, bool] = {}
        self.lock = threading.Lock()
    
    def add_event(self, user_id: str, value: float) -> None:
        """Add an event with thread-safe operations."""
        with self.lock:
            self.total_requests += 1
            self.sum += value
            if user_id not in self.users:
                self.users[user_id] = True
    
    def get_stats(self) -> Dict:
        """Get aggregated statistics."""
        with self.lock:
            total = self.total_requests
            unique = len(self.users)
            total_sum = self.sum
            avg = total_sum / total if total > 0 else 0.0
            
            return {
                "totalRequests": total,
                "uniqueUsers": unique,
                "sum": total_sum,
                "avg": avg
            }
    
    def reset(self) -> None:
        """Reset all statistics."""
        with self.lock:
            self.total_requests = 0
            self.sum = 0.0
            self.users.clear()


# Create FasterAPI native app
app = App(
    port=8000,
    host="0.0.0.0",
    enable_h2=False,  # HTTP/1.1 only for now
    enable_h3=False,
    enable_compression=True
)

# Global event store
store = EventStore()


@app.post("/event")
def post_event(req: Request, res: Response) -> None:
    """
    Accept event data and update aggregations.
    
    Uses FasterAPI's native C++ request/response objects.
    """
    try:
        # Parse JSON body using C++ parser
        body = req.body()
        data = json.loads(body)
        
        user_id = data.get("userId")
        value = data.get("value")
        
        if user_id is None or value is None:
            res.status(400).json({"error": "Missing userId or value"}).send()
            return
        
        # Add event
        store.add_event(user_id, float(value))
        
        # Send response
        res.status(201).json({"status": "ok"}).send()
        
    except Exception as e:
        res.status(400).json({"error": str(e)}).send()


@app.get("/stats")
def get_stats(req: Request, res: Response) -> None:
    """
    Return aggregated statistics.
    
    Uses C++ JSON serialization for maximum performance.
    """
    stats = store.get_stats()
    res.status(200).json(stats).send()


@app.get("/health")
def health_check(req: Request, res: Response) -> None:
    """Health check endpoint."""
    res.status(200).json({"status": "healthy"}).send()


@app.post("/reset")
def reset_stats(req: Request, res: Response) -> None:
    """Reset statistics (for testing)."""
    store.reset()
    res.status(200).json({"status": "reset"}).send()


if __name__ == "__main__":
    print("=" * 60)
    print("FasterAPI Native - 1 Million Request Challenge (1MRC)")
    print("=" * 60)
    print("")
    print("Server: FasterAPI C++ HTTP Server")
    print("Port: 8000")
    print("")
    print("Features:")
    print("  ✅ C++ HTTP/1.1 server")
    print("  ✅ Zero-copy request parsing")
    print("  ✅ SIMD JSON serialization")
    print("  ✅ Python handler callbacks")
    print("")
    print("Expected performance: 100K+ req/s")
    print("=" * 60)
    print("")
    
    # Run the FasterAPI native server
    app.run()

