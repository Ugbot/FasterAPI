"""
Pytest configuration for FastAPI compatibility tests.

This module provides fixtures and utilities for testing FasterAPI
against FastAPI to ensure drop-in compatibility.
"""

import asyncio
import os
import random
import string
import sys
import time
from contextlib import asynccontextmanager
from typing import Any, AsyncGenerator, Dict, Generator, List, Tuple
from uuid import uuid4

import pytest

# Determine which framework to test based on environment variable
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")


def get_framework():
    """Import the appropriate framework based on TEST_FRAMEWORK env var."""
    if FRAMEWORK == "fastapi":
        import fastapi

        return fastapi
    else:
        import fasterapi

        return fasterapi


def random_string(length: int = 10) -> str:
    """Generate a random string."""
    return "".join(random.choices(string.ascii_letters + string.digits, k=length))


def random_email() -> str:
    """Generate a random email address."""
    return f"{random_string(8)}@{random_string(5)}.com"


def random_int(min_val: int = 0, max_val: int = 10000) -> int:
    """Generate a random integer."""
    return random.randint(min_val, max_val)


def random_float(min_val: float = 0.0, max_val: float = 10000.0) -> float:
    """Generate a random float."""
    return random.uniform(min_val, max_val)


def random_bool() -> bool:
    """Generate a random boolean."""
    return random.choice([True, False])


def random_uuid() -> str:
    """Generate a random UUID string."""
    return str(uuid4())


def generate_user() -> Dict[str, Any]:
    """Generate a random user dictionary."""
    return {
        "id": random_uuid(),
        "username": random_string(8),
        "email": random_email(),
        "full_name": f"{random_string(6)} {random_string(8)}",
        "age": random_int(18, 80),
        "is_active": random_bool(),
        "score": random_float(0, 100),
    }


def generate_item() -> Dict[str, Any]:
    """Generate a random item dictionary."""
    return {
        "id": random_uuid(),
        "name": random_string(12),
        "description": random_string(50),
        "price": round(random_float(1, 1000), 2),
        "tax": round(random_float(0, 50), 2),
        "in_stock": random_bool(),
        "quantity": random_int(0, 100),
        "tags": [random_string(5) for _ in range(random_int(1, 5))],
    }


def generate_file_content(size_kb: int = 10) -> bytes:
    """Generate random file content."""
    return os.urandom(size_kb * 1024)


@pytest.fixture
def framework():
    """Fixture providing the current framework module."""
    return get_framework()


@pytest.fixture
def random_users() -> List[Dict[str, Any]]:
    """Fixture providing a list of random users."""
    return [generate_user() for _ in range(random_int(5, 20))]


@pytest.fixture
def random_items() -> List[Dict[str, Any]]:
    """Fixture providing a list of random items."""
    return [generate_item() for _ in range(random_int(5, 20))]


@pytest.fixture
def sample_user() -> Dict[str, Any]:
    """Fixture providing a single random user."""
    return generate_user()


@pytest.fixture
def sample_item() -> Dict[str, Any]:
    """Fixture providing a single random item."""
    return generate_item()


@pytest.fixture
def small_file() -> Tuple[str, bytes]:
    """Fixture providing a small file (1KB)."""
    return (f"{random_string(8)}.txt", generate_file_content(1))


@pytest.fixture
def medium_file() -> Tuple[str, bytes]:
    """Fixture providing a medium file (100KB)."""
    return (f"{random_string(8)}.bin", generate_file_content(100))


@pytest.fixture
def large_file() -> Tuple[str, bytes]:
    """Fixture providing a large file (1MB)."""
    return (f"{random_string(8)}.dat", generate_file_content(1024))


class BenchmarkResult:
    """Store and report benchmark results."""

    def __init__(self, name: str):
        self.name = name
        self.times: List[float] = []
        self.errors: int = 0
        self.requests: int = 0

    def record(self, duration: float, success: bool = True):
        """Record a single request result."""
        self.times.append(duration)
        self.requests += 1
        if not success:
            self.errors += 1

    @property
    def total_time(self) -> float:
        return sum(self.times)

    @property
    def avg_time(self) -> float:
        return self.total_time / len(self.times) if self.times else 0

    @property
    def min_time(self) -> float:
        return min(self.times) if self.times else 0

    @property
    def max_time(self) -> float:
        return max(self.times) if self.times else 0

    @property
    def p50(self) -> float:
        if not self.times:
            return 0
        sorted_times = sorted(self.times)
        idx = len(sorted_times) // 2
        return sorted_times[idx]

    @property
    def p95(self) -> float:
        if not self.times:
            return 0
        sorted_times = sorted(self.times)
        idx = int(len(sorted_times) * 0.95)
        return sorted_times[idx]

    @property
    def p99(self) -> float:
        if not self.times:
            return 0
        sorted_times = sorted(self.times)
        idx = int(len(sorted_times) * 0.99)
        return sorted_times[idx]

    @property
    def rps(self) -> float:
        """Requests per second."""
        return len(self.times) / self.total_time if self.total_time > 0 else 0

    def report(self) -> str:
        """Generate a text report."""
        return f"""
Benchmark: {self.name}
{"=" * 50}
Total Requests: {self.requests}
Errors: {self.errors} ({100 * self.errors / self.requests:.2f}%)
Total Time: {self.total_time:.3f}s
Requests/sec: {self.rps:.2f}

Latency (ms):
  Min: {self.min_time * 1000:.3f}
  Avg: {self.avg_time * 1000:.3f}
  Max: {self.max_time * 1000:.3f}
  P50: {self.p50 * 1000:.3f}
  P95: {self.p95 * 1000:.3f}
  P99: {self.p99 * 1000:.3f}
"""


@pytest.fixture
def benchmark_result():
    """Factory fixture for creating benchmark results."""

    def _create(name: str) -> BenchmarkResult:
        return BenchmarkResult(name)

    return _create


# Event loop fixture for async tests
@pytest.fixture(scope="session")
def event_loop():
    """Create an event loop for the test session."""
    loop = asyncio.new_event_loop()
    yield loop
    loop.close()
