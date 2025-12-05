#!/usr/bin/env python3
"""
Server-Sent Events (SSE) Demo for FasterAPI

Demonstrates SSE functionality with randomized metrics and data:
- Real-time time updates
- System metrics streaming
- Stock price simulation
- Event log streaming

Run with:
    python examples/sse_demo.py

Test with curl:
    curl http://localhost:8000/sse/time
    curl http://localhost:8000/sse/metrics
    curl http://localhost:8000/sse/stocks
    curl http://localhost:8000/sse/logs
"""

import asyncio
import random
import string
import time
import sys
from pathlib import Path
from typing import List

# Add parent directory to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from fasterapi.http.server_cy import Server
from fasterapi.http.sse import SSE, SSEStream


# ==============================================================================
# SSE Endpoints
# ==============================================================================


async def sse_time(sse: SSE):
    """
    Time endpoint: Streams server time updates.

    Sends current time every second with randomized formatting.
    """
    print(f"[Time] Client connected: {sse.connection_id}")

    try:
        formats = ["unix", "iso", "human", "detailed"]

        count = 0

        while sse.is_open and count < 300:  # Stream for 5 minutes
            count += 1
            current_time = time.time()

            # Randomize time format
            fmt = random.choice(formats)

            if fmt == "unix":
                await sse.send_json(
                    {"timestamp": current_time, "format": "unix"}, event="time"
                )

            elif fmt == "iso":
                import datetime

                dt = datetime.datetime.fromtimestamp(current_time)
                await sse.send_json(
                    {"timestamp": dt.isoformat(), "format": "iso"}, event="time"
                )

            elif fmt == "human":
                import datetime

                dt = datetime.datetime.fromtimestamp(current_time)
                await sse.send_text(dt.strftime("%Y-%m-%d %H:%M:%S"), event="time")

            else:  # detailed
                import datetime

                dt = datetime.datetime.fromtimestamp(current_time)
                await sse.send_json(
                    {
                        "timestamp": current_time,
                        "iso": dt.isoformat(),
                        "human": dt.strftime("%Y-%m-%d %H:%M:%S"),
                        "day_of_week": dt.strftime("%A"),
                        "count": count,
                    },
                    event="time",
                    event_id=str(count),
                )

            # Random interval
            await asyncio.sleep(random.uniform(0.8, 1.2))

            # Log every 30 seconds
            if count % 30 == 0:
                print(f"[Time] Connection {sse.connection_id}: {count} updates sent")

    except Exception as e:
        print(f"[Time] Error: {e}")

    finally:
        print(f"[Time] Connection {sse.connection_id} closed")
        if sse.is_open:
            await sse.close()


async def sse_metrics(sse: SSE):
    """
    Metrics endpoint: Streams randomized system metrics.

    Simulates CPU, memory, disk, and network metrics.
    """
    print(f"[Metrics] Client connected: {sse.connection_id}")

    try:
        # Use SSEStream for automatic keep-alive
        async with SSEStream(sse, keep_alive_interval=30.0) as stream:
            # Initial state
            cpu_base = random.uniform(10, 30)
            mem_base = random.uniform(40, 60)
            disk_base = random.uniform(50, 70)
            net_base = random.uniform(1, 10)  # MB/s

            count = 0

            while count < 600:  # Stream for 10 minutes
                count += 1

                # Generate randomized metrics with realistic variations
                cpu = max(0, min(100, cpu_base + random.gauss(0, 15)))
                memory = max(0, min(100, mem_base + random.gauss(0, 5)))
                disk = max(0, min(100, disk_base + random.gauss(0, 3)))
                network_rx = max(0, net_base + random.gauss(0, 5))
                network_tx = max(0, net_base * 0.5 + random.gauss(0, 2))

                # Random spike events
                if random.random() < 0.05:  # 5% chance of spike
                    cpu = min(100, cpu + random.uniform(20, 40))
                    await stream.send_json(
                        {"type": "spike", "metric": "cpu", "value": cpu}, event="alert"
                    )

                # Send metrics
                await stream.send_json(
                    {
                        "timestamp": time.time(),
                        "cpu_percent": round(cpu, 2),
                        "memory_percent": round(memory, 2),
                        "disk_percent": round(disk, 2),
                        "network_rx_mbps": round(network_rx, 2),
                        "network_tx_mbps": round(network_tx, 2),
                        "load_average": [
                            round(random.uniform(0.5, 2.0), 2) for _ in range(3)
                        ],
                        "processes": random.randint(150, 250),
                        "threads": random.randint(800, 1200),
                    },
                    event="metrics",
                    event_id=str(count),
                )

                # Slowly drift base values
                cpu_base += random.gauss(0, 0.5)
                mem_base += random.gauss(0, 0.2)
                disk_base += random.gauss(0, 0.1)
                net_base += random.gauss(0, 0.3)

                # Clamp base values
                cpu_base = max(5, min(50, cpu_base))
                mem_base = max(30, min(70, mem_base))
                disk_base = max(40, min(80, disk_base))
                net_base = max(0.5, min(20, net_base))

                # Log progress
                if count % 60 == 0:
                    print(
                        f"[Metrics] Connection {sse.connection_id}: {count} updates, "
                        f"CPU={cpu:.1f}%, Mem={memory:.1f}%"
                    )

                await asyncio.sleep(1.0)

    except Exception as e:
        print(f"[Metrics] Error: {e}")

    finally:
        print(f"[Metrics] Connection {sse.connection_id} closed")


async def sse_stocks(sse: SSE):
    """
    Stock prices endpoint: Simulates real-time stock price updates.

    Streams randomized stock prices with realistic movements.
    """
    print(f"[Stocks] Client connected: {sse.connection_id}")

    try:
        # Define mock stocks
        stocks = {
            "AAPL": 180.0,
            "GOOGL": 140.0,
            "MSFT": 380.0,
            "AMZN": 170.0,
            "TSLA": 250.0,
            "META": 350.0,
            "NVDA": 500.0,
            "AMD": 150.0,
        }

        count = 0

        while sse.is_open and count < 1000:
            count += 1

            # Pick random stock to update
            symbol = random.choice(list(stocks.keys()))
            current_price = stocks[symbol]

            # Random price movement (brownian motion)
            change_percent = random.gauss(0, 0.5)  # 0.5% standard deviation
            new_price = current_price * (1 + change_percent / 100)

            # Occasional larger movements
            if random.random() < 0.1:  # 10% chance
                change_percent = random.gauss(0, 2.0)  # 2% std dev
                new_price = current_price * (1 + change_percent / 100)

            stocks[symbol] = new_price

            # Send update
            await sse.send_json(
                {
                    "symbol": symbol,
                    "price": round(new_price, 2),
                    "change": round(new_price - current_price, 2),
                    "change_percent": round(change_percent, 3),
                    "volume": random.randint(100000, 10000000),
                    "timestamp": time.time(),
                    "bid": round(new_price - random.uniform(0.01, 0.1), 2),
                    "ask": round(new_price + random.uniform(0.01, 0.1), 2),
                },
                event="stock",
                event_id=f"{symbol}-{count}",
            )

            # Send market summary every 50 updates
            if count % 50 == 0:
                await sse.send_json(
                    {
                        "type": "summary",
                        "stocks": {k: round(v, 2) for k, v in stocks.items()},
                        "total_updates": count,
                        "timestamp": time.time(),
                    },
                    event="summary",
                )

                print(f"[Stocks] Connection {sse.connection_id}: {count} updates")

            # Random update frequency
            await asyncio.sleep(random.uniform(0.05, 0.5))

    except Exception as e:
        print(f"[Stocks] Error: {e}")

    finally:
        print(f"[Stocks] Connection {sse.connection_id} closed")
        if sse.is_open:
            await sse.close()


async def sse_logs(sse: SSE):
    """
    Log streaming endpoint: Streams randomized log events.

    Simulates application log streaming with various severity levels.
    """
    print(f"[Logs] Client connected: {sse.connection_id}")

    try:
        log_levels = ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]
        log_weights = [20, 50, 20, 8, 2]  # Weighted distribution

        components = [
            "api.auth",
            "api.users",
            "api.posts",
            "api.comments",
            "db.query",
            "db.connection",
            "cache.redis",
            "queue.worker",
            "http.server",
            "http.middleware",
            "websocket.handler",
        ]

        messages = {
            "DEBUG": [
                "Request received",
                "Processing request",
                "Cache hit",
                "Cache miss",
                "Database query executed",
            ],
            "INFO": [
                "User logged in",
                "Request completed successfully",
                "File uploaded",
                "Job queued",
                "Connection established",
            ],
            "WARNING": [
                "Slow query detected",
                "High memory usage",
                "Deprecated API used",
                "Rate limit approaching",
                "Cache eviction",
            ],
            "ERROR": [
                "Database connection failed",
                "Request timeout",
                "Invalid input data",
                "File not found",
                "Authentication failed",
            ],
            "CRITICAL": [
                "System out of memory",
                "Database unavailable",
                "Disk space critical",
                "Service unavailable",
            ],
        }

        count = 0

        while sse.is_open and count < 1000:
            count += 1

            # Random log level (weighted)
            level = random.choices(log_levels, weights=log_weights)[0]

            # Random component
            component = random.choice(components)

            # Random message
            message = random.choice(messages[level])

            # Add random context
            context = {
                "user_id": random.randint(1000, 9999),
                "request_id": "".join(random.choices(string.hexdigits.lower(), k=16)),
                "duration_ms": random.randint(10, 500),
            }

            # Send log event
            await sse.send_json(
                {
                    "timestamp": time.time(),
                    "level": level,
                    "component": component,
                    "message": message,
                    "context": context,
                },
                event="log",
                event_id=str(count),
            )

            # Send alert for errors/critical
            if level in ["ERROR", "CRITICAL"]:
                await sse.send_json(
                    {
                        "type": "alert",
                        "level": level,
                        "message": f"{component}: {message}",
                        "timestamp": time.time(),
                    },
                    event="alert",
                )

            # Log progress
            if count % 100 == 0:
                print(f"[Logs] Connection {sse.connection_id}: {count} log events")

            # Variable frequency (more logs = faster stream)
            if level == "DEBUG":
                await asyncio.sleep(random.uniform(0.1, 0.3))
            elif level in ["ERROR", "CRITICAL"]:
                await asyncio.sleep(random.uniform(1.0, 3.0))
            else:
                await asyncio.sleep(random.uniform(0.2, 0.8))

    except Exception as e:
        print(f"[Logs] Error: {e}")

    finally:
        print(f"[Logs] Connection {sse.connection_id} closed")
        if sse.is_open:
            await sse.close()


# ==============================================================================
# Server Setup
# ==============================================================================


def main():
    """Start SSE demo server."""
    # Create server
    server = Server(
        port=8000,
        host="0.0.0.0",
        enable_h2=False,
        enable_h3=False,
        enable_compression=True,
        enable_websocket=False,
    )

    # Register SSE endpoints (note: using WebSocket registration as placeholder)
    # In a full implementation, we'd have server.add_sse() method
    print("Registering SSE endpoints...")

    # For now, demonstrate the SSE classes are ready
    # The actual server integration would require additional C++ bridge work

    # Add HTTP endpoint
    def http_index(request, response):
        """HTTP index page."""
        html = """
        <html>
        <head><title>FasterAPI SSE Demo</title></head>
        <body>
            <h1>FasterAPI Server-Sent Events Demo</h1>
            <p>SSE endpoints (test with curl):</p>
            <ul>
                <li><code>curl http://localhost:8000/sse/time</code> - Time updates</li>
                <li><code>curl http://localhost:8000/sse/metrics</code> - System metrics</li>
                <li><code>curl http://localhost:8000/sse/stocks</code> - Stock prices</li>
                <li><code>curl http://localhost:8000/sse/logs</code> - Log streaming</li>
            </ul>

            <h2>JavaScript Example</h2>
            <pre>
const es = new EventSource('http://localhost:8000/sse/time');
es.addEventListener('time', (e) => {
    console.log('Time:', JSON.parse(e.data));
});
            </pre>

            <p><strong>Note:</strong> Full SSE integration requires C++ server bridge completion.</p>
        </body>
        </html>
        """
        response.status_code = 200
        response.body = html.encode("utf-8")
        response.content_type = "text/html"

    server.add_route("GET", "/", http_index)

    # Start server
    print("\nStarting FasterAPI SSE demo server...")
    print("Server listening on http://0.0.0.0:8000")
    print("\nSSE Python bindings are complete!")
    print("Waiting for C++ server SSE endpoint integration...\n")
    print("Press Ctrl+C to stop\n")

    server.start()

    try:
        # Keep server running
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.stop()
        print("Server stopped")


if __name__ == "__main__":
    main()
