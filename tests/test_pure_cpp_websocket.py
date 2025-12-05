#!/usr/bin/env python3
"""
Pure C++ WebSocket E2E Tests

Tests WebSocket functionality in pure C++ mode.
Requires: test_pure_cpp_websocket_server to be running on port 8700.

Usage:
    python3 tests/test_pure_cpp_websocket.py [--port PORT]

Or run the full test harness:
    python3 tests/test_pure_cpp_websocket.py --with-server
"""

import asyncio
import json
import os
import random
import signal
import string
import subprocess
import sys
import time
from typing import Optional

# Try to import websockets, fall back to basic socket if not available
try:
    import websockets
    HAS_WEBSOCKETS = True
except ImportError:
    HAS_WEBSOCKETS = False
    print("Note: websockets library not installed, using basic HTTP tests only")

try:
    import httpx
    HAS_HTTPX = True
except ImportError:
    HAS_HTTPX = False
    import urllib.request


DEFAULT_PORT = 8700
TIMEOUT = 10.0

# Test results tracking
passed = 0
failed = 0
results = []


def log_pass(test_name: str, details: str = ""):
    global passed
    passed += 1
    detail_str = f" - {details}" if details else ""
    print(f"  [PASS] {test_name}{detail_str}")
    results.append((test_name, True, details))


def log_fail(test_name: str, details: str = ""):
    global failed
    failed += 1
    detail_str = f" - {details}" if details else ""
    print(f"  [FAIL] {test_name}{detail_str}")
    results.append((test_name, False, details))


def random_string(length: int = 10) -> str:
    """Generate a random string for testing."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


def http_get(url: str) -> Optional[str]:
    """Make an HTTP GET request."""
    try:
        if HAS_HTTPX:
            with httpx.Client(timeout=5.0) as client:
                response = client.get(url)
                return response.text
        else:
            with urllib.request.urlopen(url, timeout=5) as response:
                return response.read().decode('utf-8')
    except Exception as e:
        return None


# ============================================
# HTTP Tests
# ============================================

def test_health_endpoint(port: int):
    """Test HTTP health endpoint."""
    url = f"http://127.0.0.1:{port}/test-health"
    response = http_get(url)

    if response is None:
        log_fail("HTTP /test-health", "No response")
        return False

    try:
        data = json.loads(response)
        if data.get("status") == "ok" and "pure_cpp" in data.get("mode", ""):
            log_pass("HTTP /test-health", f"status={data['status']}, mode={data['mode']}")
            return True
        else:
            log_fail("HTTP /test-health", f"Unexpected response: {response}")
            return False
    except json.JSONDecodeError:
        log_fail("HTTP /test-health", f"Invalid JSON: {response}")
        return False


def test_stats_endpoint(port: int):
    """Test HTTP stats endpoint."""
    url = f"http://127.0.0.1:{port}/stats"
    response = http_get(url)

    if response is None:
        log_fail("HTTP /stats", "No response")
        return False

    try:
        data = json.loads(response)
        if "connections" in data and "messages" in data:
            log_pass("HTTP /stats", f"connections={data['connections']}, messages={data['messages']}")
            return True
        else:
            log_fail("HTTP /stats", f"Missing fields: {response}")
            return False
    except json.JSONDecodeError:
        log_fail("HTTP /stats", f"Invalid JSON: {response}")
        return False


# ============================================
# WebSocket Tests
# ============================================

async def test_ws_echo_single(port: int):
    """Test single message echo on /ws/echo."""
    if not HAS_WEBSOCKETS:
        print("  [SKIP] WebSocket echo single - websockets not installed")
        return

    uri = f"ws://127.0.0.1:{port}/ws/echo"
    test_msg = f"Hello_{random_string(8)}"

    try:
        async with websockets.connect(uri, close_timeout=2) as ws:
            await ws.send(test_msg)
            response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)

            expected = f"Echo: {test_msg}"
            if response == expected:
                log_pass("WS /ws/echo single", f"'{test_msg}' -> '{response}'")
            else:
                log_fail("WS /ws/echo single", f"Expected '{expected}', got '{response}'")
    except Exception as e:
        log_fail("WS /ws/echo single", str(e))


async def test_ws_echo_multiple(port: int):
    """Test multiple sequential messages on /ws/echo."""
    if not HAS_WEBSOCKETS:
        print("  [SKIP] WebSocket echo multiple - websockets not installed")
        return

    uri = f"ws://127.0.0.1:{port}/ws/echo"
    num_messages = 5
    messages = [f"Msg{i}_{random_string(6)}" for i in range(num_messages)]

    try:
        async with websockets.connect(uri, close_timeout=2) as ws:
            all_correct = True
            for msg in messages:
                await ws.send(msg)
                response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)
                expected = f"Echo: {msg}"
                if response != expected:
                    log_fail("WS /ws/echo multiple", f"Message mismatch: expected '{expected}', got '{response}'")
                    all_correct = False
                    break

            if all_correct:
                log_pass("WS /ws/echo multiple", f"All {num_messages} messages echoed correctly")
    except Exception as e:
        log_fail("WS /ws/echo multiple", str(e))


async def test_ws_echo_binary(port: int):
    """Test binary message echo on /ws/echo."""
    if not HAS_WEBSOCKETS:
        print("  [SKIP] WebSocket echo binary - websockets not installed")
        return

    uri = f"ws://127.0.0.1:{port}/ws/echo"
    test_data = bytes([random.randint(0, 255) for _ in range(64)])

    try:
        async with websockets.connect(uri, close_timeout=2) as ws:
            await ws.send(test_data)
            response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)

            if isinstance(response, bytes) and response == test_data:
                log_pass("WS /ws/echo binary", f"64 bytes echoed correctly")
            else:
                log_fail("WS /ws/echo binary", f"Binary data mismatch")
    except Exception as e:
        log_fail("WS /ws/echo binary", str(e))


async def test_ws_uppercase(port: int):
    """Test uppercase conversion on /ws/uppercase."""
    if not HAS_WEBSOCKETS:
        print("  [SKIP] WebSocket uppercase - websockets not installed")
        return

    uri = f"ws://127.0.0.1:{port}/ws/uppercase"
    test_cases = [
        ("hello", "HELLO"),
        ("Hello World", "HELLO WORLD"),
        ("Test123", "TEST123"),
        (random_string(10).lower(), None),  # Generated, will check uppercase
    ]

    try:
        async with websockets.connect(uri, close_timeout=2) as ws:
            all_correct = True
            for msg, expected in test_cases:
                await ws.send(msg)
                response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)

                if expected is None:
                    expected = msg.upper()

                if response != expected:
                    log_fail("WS /ws/uppercase", f"'{msg}' -> '{response}', expected '{expected}'")
                    all_correct = False
                    break

            if all_correct:
                log_pass("WS /ws/uppercase", f"All {len(test_cases)} conversions correct")
    except Exception as e:
        log_fail("WS /ws/uppercase", str(e))


async def test_ws_reverse(port: int):
    """Test string reversal on /ws/reverse."""
    if not HAS_WEBSOCKETS:
        print("  [SKIP] WebSocket reverse - websockets not installed")
        return

    uri = f"ws://127.0.0.1:{port}/ws/reverse"
    test_cases = [
        "hello",
        "racecar",  # Palindrome
        random_string(15),
    ]

    try:
        async with websockets.connect(uri, close_timeout=2) as ws:
            all_correct = True
            for msg in test_cases:
                await ws.send(msg)
                response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)
                expected = msg[::-1]

                if response != expected:
                    log_fail("WS /ws/reverse", f"'{msg}' -> '{response}', expected '{expected}'")
                    all_correct = False
                    break

            if all_correct:
                log_pass("WS /ws/reverse", f"All {len(test_cases)} reversals correct")
    except Exception as e:
        log_fail("WS /ws/reverse", str(e))


async def test_ws_json(port: int):
    """Test JSON wrapping on /ws/json."""
    if not HAS_WEBSOCKETS:
        print("  [SKIP] WebSocket JSON - websockets not installed")
        return

    uri = f"ws://127.0.0.1:{port}/ws/json"
    test_msg = f"test_{random_string(8)}"

    try:
        async with websockets.connect(uri, close_timeout=2) as ws:
            await ws.send(test_msg)
            response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)

            try:
                data = json.loads(response)
                if data.get("message") == test_msg and "id" in data:
                    log_pass("WS /ws/json", f"JSON wrapped: id={data['id']}, message={data['message']}")
                else:
                    log_fail("WS /ws/json", f"Unexpected JSON: {response}")
            except json.JSONDecodeError:
                log_fail("WS /ws/json", f"Invalid JSON response: {response}")
    except Exception as e:
        log_fail("WS /ws/json", str(e))


async def test_ws_rapid_messages(port: int):
    """Test rapid message sending (stress test)."""
    if not HAS_WEBSOCKETS:
        print("  [SKIP] WebSocket rapid messages - websockets not installed")
        return

    uri = f"ws://127.0.0.1:{port}/ws/echo"
    num_messages = 20

    try:
        async with websockets.connect(uri, close_timeout=5) as ws:
            # Send all messages quickly
            messages = [f"Rapid{i}_{random_string(4)}" for i in range(num_messages)]
            for msg in messages:
                await ws.send(msg)

            # Receive all responses
            received = []
            for _ in range(num_messages):
                response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)
                received.append(response)

            # Verify all received correctly
            expected = [f"Echo: {msg}" for msg in messages]
            if received == expected:
                log_pass("WS rapid messages", f"All {num_messages} rapid messages echoed correctly")
            else:
                mismatches = sum(1 for a, b in zip(received, expected) if a != b)
                log_fail("WS rapid messages", f"{mismatches}/{num_messages} mismatches")
    except Exception as e:
        log_fail("WS rapid messages", str(e))


async def test_ws_concurrent_connections(port: int):
    """Test multiple concurrent WebSocket connections."""
    if not HAS_WEBSOCKETS:
        print("  [SKIP] WebSocket concurrent - websockets not installed")
        return

    uri = f"ws://127.0.0.1:{port}/ws/echo"
    num_connections = 3

    async def connection_task(conn_id: int):
        async with websockets.connect(uri, close_timeout=2) as ws:
            msg = f"Conn{conn_id}_{random_string(6)}"
            await ws.send(msg)
            response = await asyncio.wait_for(ws.recv(), timeout=TIMEOUT)
            expected = f"Echo: {msg}"
            return response == expected

    try:
        tasks = [connection_task(i) for i in range(num_connections)]
        results_list = await asyncio.gather(*tasks, return_exceptions=True)

        successes = sum(1 for r in results_list if r is True)
        if successes == num_connections:
            log_pass("WS concurrent connections", f"All {num_connections} connections successful")
        else:
            log_fail("WS concurrent connections", f"{successes}/{num_connections} succeeded")
    except Exception as e:
        log_fail("WS concurrent connections", str(e))


async def run_websocket_tests(port: int):
    """Run all WebSocket tests."""
    await test_ws_echo_single(port)
    await test_ws_echo_multiple(port)
    await test_ws_echo_binary(port)
    await test_ws_uppercase(port)
    await test_ws_reverse(port)
    await test_ws_json(port)
    await test_ws_rapid_messages(port)
    await test_ws_concurrent_connections(port)


# ============================================
# Test Runner
# ============================================

def wait_for_server(port: int, timeout: float = 15.0) -> bool:
    """Wait for server to be ready."""
    start = time.time()
    while time.time() - start < timeout:
        response = http_get(f"http://127.0.0.1:{port}/health")
        if response:
            try:
                data = json.loads(response)
                # Accept either "ok" or "healthy" status (built-in returns "healthy")
                if data.get("status") in ("ok", "healthy"):
                    return True
            except:
                pass
        time.sleep(0.5)
    return False


def run_tests(port: int):
    """Run all tests."""
    global passed, failed

    print(f"\n{'='*60}")
    print(f"Pure C++ WebSocket Tests")
    print(f"Port: {port}")
    print(f"{'='*60}\n")

    # HTTP tests
    print("--- HTTP Endpoint Tests ---")
    test_health_endpoint(port)
    test_stats_endpoint(port)

    # WebSocket tests
    if HAS_WEBSOCKETS:
        print("\n--- WebSocket Tests ---")
        asyncio.run(run_websocket_tests(port))
    else:
        print("\n--- WebSocket Tests (SKIPPED - install websockets library) ---")

    # Summary
    print(f"\n{'='*60}")
    print(f"Results: {passed} passed, {failed} failed")
    print(f"{'='*60}\n")

    return failed == 0


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Pure C++ WebSocket E2E Tests")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Server port")
    parser.add_argument("--with-server", action="store_true", help="Start server automatically")
    args = parser.parse_args()

    server_process = None

    try:
        if args.with_server:
            print("Starting test server...")

            # Build the server first
            build_cmd = ["cmake", "--build", "build", "--target", "test_pure_cpp_websocket_server"]
            subprocess.run(build_cmd, check=True, capture_output=True)

            # Start the server
            env = os.environ.copy()
            env["DYLD_LIBRARY_PATH"] = f"build/lib:{env.get('DYLD_LIBRARY_PATH', '')}"

            server_cmd = [f"./build/tests/test_pure_cpp_websocket_server", str(args.port)]
            server_process = subprocess.Popen(
                server_cmd,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )

            # Wait for SERVER_READY signal
            print("Waiting for server to start...")
            if not wait_for_server(args.port):
                print("ERROR: Server failed to start")
                return 1
            print("Server ready!")

        # Run tests
        success = run_tests(args.port)
        return 0 if success else 1

    except KeyboardInterrupt:
        print("\nInterrupted")
        return 1
    finally:
        if server_process:
            server_process.terminate()
            try:
                server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server_process.kill()


if __name__ == "__main__":
    sys.exit(main())
