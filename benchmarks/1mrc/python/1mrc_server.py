"""
FasterAPI implementation of the 1 Million Request Challenge (1MRC)

Challenge: https://github.com/Kavishankarks/1mrc

Requirements:
- POST /event - Accept event data (userId, value)
- GET /stats - Return aggregated statistics
- Handle 1,000,000 concurrent requests
- Maintain thread safety
- Maximize throughput
"""

from fastapi import FastAPI
from pydantic import BaseModel
from threading import Lock
from typing import Dict
import time


class EventRequest(BaseModel):
    """Event data model."""
    userId: str
    value: float


class StatsResponse(BaseModel):
    """Statistics response model."""
    totalRequests: int
    uniqueUsers: int
    sum: float
    avg: float


class EventStore:
    """
    Thread-safe event storage with atomic operations.
    
    Uses Python's GIL for thread safety with explicit locks
    for critical sections to prevent race conditions.
    """
    
    def __init__(self):
        self.total_requests: int = 0
        self.sum: float = 0.0
        self.users: Dict[str, bool] = {}
        self.lock = Lock()
    
    def add_event(self, user_id: str, value: float) -> None:
        """
        Add an event with thread-safe atomic operations.
        
        Args:
            user_id: Unique user identifier
            value: Numeric value to aggregate
        """
        with self.lock:
            self.total_requests += 1
            self.sum += value
            if user_id not in self.users:
                self.users[user_id] = True
    
    def get_stats(self) -> StatsResponse:
        """
        Get aggregated statistics.
        
        Returns:
            Statistics with total requests, unique users, sum, and average
        """
        with self.lock:
            total = self.total_requests
            unique = len(self.users)
            total_sum = self.sum
            avg = total_sum / total if total > 0 else 0.0
            
            return StatsResponse(
                totalRequests=total,
                uniqueUsers=unique,
                sum=total_sum,
                avg=avg
            )
    
    def reset(self) -> None:
        """Reset all statistics (for testing)."""
        with self.lock:
            self.total_requests = 0
            self.sum = 0.0
            self.users.clear()


# Create FastAPI app with optimized settings
app = FastAPI(
    title="FasterAPI 1MRC",
    description="1 Million Request Challenge implementation using FasterAPI",
    version="1.0.0",
    docs_url="/docs",
    redoc_url="/redoc"
)

# Global event store
store = EventStore()


@app.post("/event", status_code=201)
async def post_event(event: EventRequest) -> Dict[str, str]:
    """
    Accept event data and update aggregations.
    
    Thread-safe event processing with atomic operations.
    """
    store.add_event(event.userId, event.value)
    return {"status": "ok"}


@app.get("/stats", response_model=StatsResponse)
async def get_stats() -> StatsResponse:
    """
    Return aggregated statistics.
    
    Returns current state of all processed events.
    """
    return store.get_stats()


@app.get("/health")
async def health_check() -> Dict[str, str]:
    """Health check endpoint."""
    return {"status": "healthy"}


@app.post("/reset")
async def reset_stats() -> Dict[str, str]:
    """Reset statistics (for testing purposes)."""
    store.reset()
    return {"status": "reset"}


if __name__ == "__main__":
    import uvicorn
    
    print("=" * 60)
    print("FasterAPI - 1 Million Request Challenge (1MRC)")
    print("=" * 60)
    print("")
    print("Starting server on http://0.0.0.0:8000")
    print("")
    print("Endpoints:")
    print("  POST /event  - Accept event data")
    print("  GET  /stats  - Get aggregated statistics")
    print("  GET  /health - Health check")
    print("  POST /reset  - Reset statistics")
    print("")
    print("Ready to handle 1,000,000 requests! 🚀")
    print("=" * 60)
    
    # Run with optimized uvicorn settings
    uvicorn.run(
        app,
        host="0.0.0.0",
        port=8000,
        workers=1,  # Single worker for thread-safe testing
        log_level="warning",  # Reduce logging overhead
        access_log=False,  # Disable access logs for performance
        limit_concurrency=10000,  # High concurrency limit
        backlog=2048  # Large connection backlog
    )

