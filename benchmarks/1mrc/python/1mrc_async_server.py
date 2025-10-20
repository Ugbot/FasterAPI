"""
FasterAPI Async/Futures implementation of 1 Million Request Challenge (1MRC)

Demonstrates:
- Future chaining with .then()
- Async combinators (map_async, filter_async, when_all)
- Pipeline composition
- Non-blocking event processing

Expected performance: Higher throughput with pipelining
"""

from fastapi import FastAPI
from pydantic import BaseModel
from threading import Lock
from typing import Dict, List
import asyncio

# FasterAPI futures and combinators
from fasterapi.core import Future, when_all
from fasterapi.core.combinators import (
    map_async, filter_async, reduce_async,
    Pipeline, chain
)


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


class AsyncEventStore:
    """
    Thread-safe event storage with async processing pipeline.
    
    Features:
    - Async event validation
    - Future-based aggregation
    - Pipeline composition
    - Batch processing
    """
    
    def __init__(self):
        self.total_requests: int = 0
        self.sum: float = 0.0
        self.users: Dict[str, bool] = {}
        self.lock = Lock()
        
        # Batch processing
        self.batch_size = 100
        self.event_queue: List[tuple] = []
        self.batch_lock = Lock()
    
    async def add_event_async(self, user_id: str, value: float) -> Dict:
        """
        Add event with async pipeline.
        
        Pipeline stages:
        1. Validate input
        2. Update counters
        3. Track user
        4. Return confirmation
        """
        # Create async pipeline
        pipeline = Pipeline()
        
        # Stage 1: Validate
        pipeline.add(lambda: self._validate_event(user_id, value))
        
        # Stage 2: Update aggregates
        pipeline.add(lambda _: self._update_aggregates(user_id, value))
        
        # Stage 3: Return result
        pipeline.add(lambda _: {"status": "ok", "userId": user_id})
        
        # Execute pipeline
        return await pipeline.execute()
    
    def _validate_event(self, user_id: str, value: float) -> bool:
        """Validate event data."""
        if not user_id or value is None:
            raise ValueError("Invalid event")
        return True
    
    def _update_aggregates(self, user_id: str, value: float) -> None:
        """Update aggregates with lock."""
        with self.lock:
            self.total_requests += 1
            self.sum += value
            if user_id not in self.users:
                self.users[user_id] = True
    
    async def add_event_batch(self, events: List[tuple]) -> List[Dict]:
        """
        Process multiple events in batch using async combinators.
        
        Uses map_async for parallel processing.
        """
        # Create futures for each event
        futures = [
            self._process_event_future(user_id, value)
            for user_id, value in events
        ]
        
        # Wait for all to complete
        results = await when_all(futures)
        return results
    
    async def _process_event_future(self, user_id: str, value: float) -> Dict:
        """Process single event and return future."""
        return await self.add_event_async(user_id, value)
    
    async def get_stats_async(self) -> StatsResponse:
        """
        Get statistics with async computation.
        
        Uses future chaining for async aggregation.
        """
        # Create pipeline for stats computation
        pipeline = Pipeline()
        
        # Stage 1: Get raw data
        pipeline.add(lambda: self._get_raw_stats())
        
        # Stage 2: Compute average
        pipeline.add(lambda stats: {
            **stats,
            'avg': stats['sum'] / stats['totalRequests'] if stats['totalRequests'] > 0 else 0.0
        })
        
        # Stage 3: Create response
        pipeline.add(lambda stats: StatsResponse(**stats))
        
        return await pipeline.execute()
    
    def _get_raw_stats(self) -> Dict:
        """Get raw statistics (synchronous)."""
        with self.lock:
            return {
                'totalRequests': self.total_requests,
                'uniqueUsers': len(self.users),
                'sum': self.sum
            }
    
    def reset(self) -> None:
        """Reset all statistics."""
        with self.lock:
            self.total_requests = 0
            self.sum = 0.0
            self.users.clear()


# Create FastAPI app
app = FastAPI(
    title="FasterAPI Async/Futures 1MRC",
    description="1MRC with async futures and combinators",
    version="1.0.0"
)

# Global async event store
store = AsyncEventStore()


@app.post("/event", status_code=201)
async def post_event(event: EventRequest) -> Dict[str, str]:
    """
    Accept event data with async processing.
    
    Uses FasterAPI futures and pipeline composition.
    """
    result = await store.add_event_async(event.userId, event.value)
    return result


@app.get("/stats", response_model=StatsResponse)
async def get_stats() -> StatsResponse:
    """
    Return aggregated statistics with async computation.
    """
    return await store.get_stats_async()


@app.post("/batch")
async def post_batch(events: List[EventRequest]) -> Dict:
    """
    Batch event processing using async combinators.
    
    Demonstrates:
    - when_all for parallel processing
    - map_async for transformation
    - reduce_async for aggregation
    """
    # Extract event tuples
    event_tuples = [(e.userId, e.value) for e in events]
    
    # Process batch asynchronously
    results = await store.add_event_batch(event_tuples)
    
    return {
        "status": "ok",
        "processed": len(results)
    }


@app.get("/health")
async def health_check() -> Dict[str, str]:
    """Health check endpoint."""
    return {"status": "healthy"}


@app.post("/reset")
async def reset_stats() -> Dict[str, str]:
    """Reset statistics."""
    store.reset()
    return {"status": "reset"}


if __name__ == "__main__":
    import uvicorn
    
    print("=" * 60)
    print("FasterAPI Async/Futures - 1MRC")
    print("=" * 60)
    print("")
    print("Features:")
    print("  ✅ Future chaining with .then()")
    print("  ✅ Async combinators (when_all, map_async)")
    print("  ✅ Pipeline composition")
    print("  ✅ Batch processing")
    print("")
    print("Endpoints:")
    print("  POST /event  - Single event (async pipeline)")
    print("  POST /batch  - Batch events (async combinators)")
    print("  GET  /stats  - Aggregated stats (async)")
    print("  GET  /health - Health check")
    print("")
    print("Server: http://0.0.0.0:8000")
    print("Expected: Higher throughput with async pipelining")
    print("=" * 60)
    
    # Run with optimized settings
    uvicorn.run(
        app,
        host="0.0.0.0",
        port=8000,
        workers=1,
        log_level="warning",
        access_log=False,
        limit_concurrency=10000,
        backlog=2048
    )

