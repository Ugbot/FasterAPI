# FasterAPI Production Deployment Guide

## Overview

This guide covers deploying FasterAPI applications with Seastar-style futures in production environments.

## Table of Contents

1. [Performance Tuning](#performance-tuning)
2. [Monitoring & Observability](#monitoring--observability)
3. [Error Handling](#error-handling)
4. [Deployment Strategies](#deployment-strategies)
5. [Scaling Guidelines](#scaling-guidelines)
6. [Best Practices](#best-practices)

## Performance Tuning

### Reactor Configuration

```python
from fasterapi.core import Reactor

# Initialize reactor with specific core count
Reactor.initialize(num_cores=8)  # Use 8 cores

# Or auto-detect
Reactor.initialize()  # Uses all available cores
```

**Recommendations:**
- Use 1 reactor per physical CPU core
- Avoid over-subscription (more reactors than cores)
- Pin reactors to specific cores for NUMA systems

### Connection Pooling

```python
from fasterapi.pg import PgPool

# Configure pool sizes based on load
pool = PgPool(
    dsn="postgres://...",
    min_connections=2,      # Min per core
    max_connections=10,     # Max per core
    idle_timeout_secs=600,  # Close idle after 10min
    health_check_interval_secs=30
)
```

**Sizing Guidelines:**
- Min connections: 1-2 per core (always ready)
- Max connections: 5-20 per core (based on load)
- Total connections = max_connections × num_cores
- PostgreSQL `max_connections` should be > total

### Async vs Sync Decision

Use **async/await** (default):
```python
@app.get("/user/{id}")
async def get_user(id: int):
    user = await db.get_async(id)  # ~0.7 µs overhead
    return user
```

Use **explicit chains** for hot paths:
```python
@app.get("/metrics")
def get_metrics():
    return (fetch_metrics()      # ~0.5 µs overhead
            .then(aggregate)
            .then(format_json))
```

**Decision Matrix:**

| Scenario | Use | Reason |
|----------|-----|--------|
| Business logic | async/await | Readability |
| Hot path (>1000 req/s) | explicit chains | -30% overhead |
| I/O heavy | async/await | Natural flow |
| CPU heavy | sync | No async benefit |

## Monitoring & Observability

### Built-in Metrics

```python
from fasterapi.core import Reactor

# Get reactor stats
stats = Reactor.get(0).get_stats()
print(f"Tasks executed: {stats.tasks_executed}")
print(f"Tasks pending: {stats.tasks_pending}")
print(f"I/O events: {stats.io_events}")
```

### Custom Metrics

```python
import time
from typing import Callable

class MetricsMiddleware:
    """Track request metrics."""
    
    def __init__(self):
        self.request_count = 0
        self.total_duration = 0.0
    
    def __call__(self, req, res):
        start = time.perf_counter()
        self.request_count += 1
        
        # Store original send
        original_send = res.send
        
        def send_with_timing(*args, **kwargs):
            duration = time.perf_counter() - start
            self.total_duration += duration
            res.headers["X-Response-Time"] = f"{duration*1000:.2f}ms"
            return original_send(*args, **kwargs)
        
        res.send = send_with_timing

# Apply middleware
app.add_middleware(MetricsMiddleware())
```

### Logging Best Practices

```python
import logging
import json

# Structured logging
logger = logging.getLogger("fasterapi")
logger.setLevel(logging.INFO)

@app.get("/user/{id}")
async def get_user(id: int):
    logger.info(
        "get_user_request",
        extra={
            "user_id": id,
            "core": Reactor.current_core(),
            "timestamp": time.time()
        }
    )
    
    user = await db.get_async(id)
    
    logger.info(
        "get_user_response",
        extra={"user_id": id, "found": user is not None}
    )
    
    return user
```

## Error Handling

### Retry Patterns

```python
from fasterapi.core.combinators import retry_async

@app.post("/transaction")
async def process_transaction(data):
    # Retry with exponential backoff
    result = await retry_async(
        lambda: db.exec_async("BEGIN; ...; COMMIT"),
        max_retries=3,
        delay=0.1,        # Start with 100ms
        backoff=2.0       # Double each retry
    )
    return result
```

### Timeout Protection

```python
from fasterapi.core.combinators import timeout_async

@app.get("/external-api")
async def call_external():
    try:
        result = await timeout_async(
            external_api_call(),
            timeout_seconds=5.0
        )
        return result
    except asyncio.TimeoutError:
        return {"error": "Request timeout"}, 504
```

### Circuit Breaker Pattern

```python
class CircuitBreaker:
    """Simple circuit breaker."""
    
    def __init__(self, failure_threshold=5, timeout=60):
        self.failure_count = 0
        self.failure_threshold = failure_threshold
        self.timeout = timeout
        self.last_failure_time = 0
        self.state = "closed"  # closed, open, half-open
    
    async def call(self, func):
        if self.state == "open":
            if time.time() - self.last_failure_time > self.timeout:
                self.state = "half-open"
            else:
                raise Exception("Circuit breaker is OPEN")
        
        try:
            result = await func()
            if self.state == "half-open":
                self.state = "closed"
                self.failure_count = 0
            return result
        except Exception as e:
            self.failure_count += 1
            self.last_failure_time = time.time()
            
            if self.failure_count >= self.failure_threshold:
                self.state = "open"
            
            raise e

# Usage
db_breaker = CircuitBreaker(failure_threshold=5, timeout=60)

@app.get("/user/{id}")
async def get_user(id: int):
    return await db_breaker.call(
        lambda: db.get_async(id)
    )
```

## Deployment Strategies

### Docker Deployment

```dockerfile
# Dockerfile
FROM python:3.11-slim

WORKDIR /app

# Install system dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy application
COPY . /app

# Build C++ libraries
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# Install Python dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Run application
CMD ["python", "app.py"]
```

### Kubernetes Deployment

```yaml
# deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: fasterapi-app
spec:
  replicas: 3
  selector:
    matchLabels:
      app: fasterapi
  template:
    metadata:
      labels:
        app: fasterapi
    spec:
      containers:
      - name: app
        image: fasterapi:latest
        ports:
        - containerPort: 8000
        env:
        - name: DATABASE_URL
          valueFrom:
            secretKeyRef:
              name: db-secret
              key: url
        resources:
          requests:
            cpu: "1000m"
            memory: "512Mi"
          limits:
            cpu: "2000m"
            memory: "1Gi"
        livenessProbe:
          httpGet:
            path: /health
            port: 8000
          initialDelaySeconds: 10
          periodSeconds: 5
        readinessProbe:
          httpGet:
            path: /ready
            port: 8000
          initialDelaySeconds: 5
          periodSeconds: 3
```

### Systemd Service

```ini
# /etc/systemd/system/fasterapi.service
[Unit]
Description=FasterAPI Application
After=network.target postgresql.service

[Service]
Type=simple
User=fasterapi
WorkingDirectory=/opt/fasterapi
Environment="PYTHONPATH=/opt/fasterapi"
ExecStart=/usr/bin/python3 /opt/fasterapi/app.py
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=multi-user.target
```

## Scaling Guidelines

### Horizontal Scaling

**Load Balancer Configuration:**
```nginx
# nginx.conf
upstream fasterapi {
    least_conn;  # Use least connections
    server app1:8000 max_fails=3 fail_timeout=30s;
    server app2:8000 max_fails=3 fail_timeout=30s;
    server app3:8000 max_fails=3 fail_timeout=30s;
}

server {
    listen 80;
    
    location / {
        proxy_pass http://fasterapi;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        
        # Enable keepalive
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        
        # Timeouts
        proxy_connect_timeout 5s;
        proxy_send_timeout 60s;
        proxy_read_timeout 60s;
    }
}
```

### Vertical Scaling

**Reactor Tuning:**
```python
import os

# Scale based on CPU cores
num_cores = int(os.getenv("REACTOR_CORES", "0"))
Reactor.initialize(num_cores=num_cores)

# Pool sizing scales with cores
pool_size_per_core = 10
pool = PgPool(
    dsn=os.getenv("DATABASE_URL"),
    max_connections=pool_size_per_core
)
```

## Best Practices

### 1. Use Dependency Injection

```python
# Good: Testable, configurable
@app.get("/user/{id}")
async def get_user(id: int, db = Depends(get_db)):
    return await db.get_async(id)

# Bad: Hard-coded dependencies
@app.get("/user/{id}")
async def get_user(id: int):
    db = Database()  # Creates new instance each time!
    return await db.get_async(id)
```

### 2. Batch Database Queries

```python
# Good: Single parallel query
user_futures = [db.get_async(id) for id in user_ids]
users = await when_all(user_futures)

# Bad: Sequential queries
users = []
for id in user_ids:
    users.append(await db.get_async(id))  # Slow!
```

### 3. Cache Strategically

```python
# Cache expensive computations
@functools.lru_cache(maxsize=1000)
def expensive_computation(key):
    # ...
    return result

# Cache database results
async def get_user_cached(id: int):
    cached = await cache.get(f"user:{id}")
    if cached:
        return cached
    
    user = await db.get_async(id)
    await cache.set(f"user:{id}", user, ttl=300)
    return user
```

### 4. Handle Backpressure

```python
from asyncio import Semaphore

# Limit concurrent operations
semaphore = Semaphore(100)  # Max 100 concurrent

@app.post("/process")
async def process_item(item):
    async with semaphore:
        return await heavy_processing(item)
```

### 5. Monitor Resource Usage

```python
import psutil

@app.get("/health")
def health_check():
    return {
        "status": "healthy",
        "cpu_percent": psutil.cpu_percent(),
        "memory_percent": psutil.virtual_memory().percent,
        "reactor_cores": Reactor.num_cores(),
        "active_connections": pool.stats()
    }
```

## Performance Checklist

- [ ] Reactor initialized with optimal core count
- [ ] Connection pool sized appropriately
- [ ] Async used for I/O-bound operations
- [ ] Explicit chains used for hot paths (>1000 req/s)
- [ ] Retry logic with exponential backoff
- [ ] Timeout protection on external calls
- [ ] Circuit breakers for failing dependencies
- [ ] Caching for expensive operations
- [ ] Metrics collection enabled
- [ ] Structured logging configured
- [ ] Health check endpoint implemented
- [ ] Resource limits set (Docker/K8s)
- [ ] Load testing completed
- [ ] Monitoring dashboards created

## Troubleshooting

### High Latency

1. Check reactor task queue: `stats.tasks_pending`
2. Profile async operations with timing
3. Look for blocking I/O in async paths
4. Verify connection pool isn't exhausted

### Memory Leaks

1. Monitor with `psutil.Process().memory_info()`
2. Check for unclosed database connections
3. Verify future chains are completing
4. Look for growing caches without TTL

### Connection Pool Exhaustion

1. Increase `max_connections` per core
2. Reduce query duration
3. Add connection timeout
4. Implement connection retry logic

## Additional Resources

- [ASYNC_FEATURES.md](ASYNC_FEATURES.md) - API reference
- [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) - Technical details
- [examples/production_app.py](examples/production_app.py) - Full example

---

**Last Updated:** October 18, 2025

