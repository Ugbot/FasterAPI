"""
Rate Limiting Middleware for FasterAPI.

Production-grade rate limiting with multiple algorithms:
- Token bucket: Smooth rate limiting with burst support
- Sliding window: Fixed time window with smooth rollover
- Fixed window: Simple counter per time window

Usage:
    from fasterapi import FastAPI
    from fasterapi.middleware import RateLimitMiddleware

    app = FastAPI()

    app.add_middleware(
        RateLimitMiddleware,
        requests_per_minute=100,
        algorithm="token_bucket",
        burst_size=10
    )
"""

import time
from collections import defaultdict
from dataclasses import dataclass
from enum import Enum
from threading import Lock
from typing import Any, Callable, Dict, List, Optional, Set, Union


class RateLimitAlgorithm(Enum):
    """Rate limiting algorithm types."""

    TOKEN_BUCKET = "token_bucket"
    SLIDING_WINDOW = "sliding_window"
    FIXED_WINDOW = "fixed_window"


@dataclass
class RateLimitResult:
    """Result of a rate limit check."""

    allowed: bool
    remaining: int
    limit: int
    reset_at: float  # Unix timestamp
    retry_after: float = 0.0  # Seconds until retry allowed


class TokenBucket:
    """
    Token bucket rate limiter.

    Provides smooth rate limiting with burst capacity.
    Tokens are replenished at a constant rate.
    """

    def __init__(
        self,
        rate: float,
        capacity: int,
    ):
        """
        Initialize token bucket.

        Args:
            rate: Tokens added per second
            capacity: Maximum tokens (burst size)
        """
        self.rate = rate
        self.capacity = capacity
        self._tokens: Dict[str, float] = {}
        self._last_update: Dict[str, float] = {}
        self._lock = Lock()

    def consume(self, key: str, tokens: int = 1) -> RateLimitResult:
        """
        Try to consume tokens from the bucket.

        Args:
            key: Rate limit key (e.g., IP address)
            tokens: Number of tokens to consume

        Returns:
            RateLimitResult with allowed status and metadata
        """
        now = time.time()

        with self._lock:
            # Initialize bucket if new key
            if key not in self._tokens:
                self._tokens[key] = float(self.capacity)
                self._last_update[key] = now

            # Replenish tokens based on elapsed time
            elapsed = now - self._last_update[key]
            self._tokens[key] = min(
                self.capacity, self._tokens[key] + (elapsed * self.rate)
            )
            self._last_update[key] = now

            # Try to consume
            if self._tokens[key] >= tokens:
                self._tokens[key] -= tokens
                return RateLimitResult(
                    allowed=True,
                    remaining=int(self._tokens[key]),
                    limit=self.capacity,
                    reset_at=now + (self.capacity / self.rate),
                )

            # Calculate retry time
            tokens_needed = tokens - self._tokens[key]
            retry_after = tokens_needed / self.rate

            return RateLimitResult(
                allowed=False,
                remaining=0,
                limit=self.capacity,
                reset_at=now + retry_after,
                retry_after=retry_after,
            )

    def reset(self, key: str) -> None:
        """Reset rate limit for a key."""
        with self._lock:
            self._tokens.pop(key, None)
            self._last_update.pop(key, None)

    def clear(self) -> None:
        """Clear all rate limit state."""
        with self._lock:
            self._tokens.clear()
            self._last_update.clear()


class SlidingWindow:
    """
    Sliding window rate limiter.

    Provides smooth rate limiting using time-based windows.
    More accurate than fixed window, prevents boundary burst.
    """

    def __init__(
        self,
        limit: int,
        window_seconds: float,
        granularity: int = 10,
    ):
        """
        Initialize sliding window.

        Args:
            limit: Maximum requests per window
            window_seconds: Window size in seconds
            granularity: Number of sub-windows for smoothness
        """
        self.limit = limit
        self.window_seconds = window_seconds
        self.granularity = granularity
        self.sub_window_seconds = window_seconds / granularity
        self._counts: Dict[str, List[int]] = defaultdict(lambda: [0] * granularity)
        self._window_start: Dict[str, float] = {}
        self._lock = Lock()

    def consume(self, key: str, count: int = 1) -> RateLimitResult:
        """
        Try to consume from the window.

        Args:
            key: Rate limit key
            count: Number of requests to record

        Returns:
            RateLimitResult with allowed status and metadata
        """
        now = time.time()

        with self._lock:
            # Initialize window if new key
            if key not in self._window_start:
                self._window_start[key] = now
                self._counts[key] = [0] * self.granularity

            window_start = self._window_start[key]

            # Check if window has completely expired
            if now >= window_start + self.window_seconds:
                # Reset window
                self._window_start[key] = now
                self._counts[key] = [0] * self.granularity
                window_start = now

            # Calculate current sub-window index
            elapsed = now - window_start
            current_idx = int(elapsed / self.sub_window_seconds) % self.granularity

            # Count total requests in window
            total = sum(self._counts[key])

            if total + count <= self.limit:
                # Allowed - record request
                self._counts[key][current_idx] += count

                return RateLimitResult(
                    allowed=True,
                    remaining=self.limit - total - count,
                    limit=self.limit,
                    reset_at=window_start + self.window_seconds,
                )

            # Rate limited
            return RateLimitResult(
                allowed=False,
                remaining=0,
                limit=self.limit,
                reset_at=window_start + self.window_seconds,
                retry_after=window_start + self.window_seconds - now,
            )

    def reset(self, key: str) -> None:
        """Reset rate limit for a key."""
        with self._lock:
            self._counts.pop(key, None)
            self._window_start.pop(key, None)

    def clear(self) -> None:
        """Clear all rate limit state."""
        with self._lock:
            self._counts.clear()
            self._window_start.clear()


class FixedWindow:
    """
    Fixed window rate limiter.

    Simple counter per time window. May allow burst at window boundaries.
    """

    def __init__(
        self,
        limit: int,
        window_seconds: float,
    ):
        """
        Initialize fixed window.

        Args:
            limit: Maximum requests per window
            window_seconds: Window size in seconds
        """
        self.limit = limit
        self.window_seconds = window_seconds
        self._counts: Dict[str, int] = {}
        self._window_start: Dict[str, float] = {}
        self._lock = Lock()

    def consume(self, key: str, count: int = 1) -> RateLimitResult:
        """
        Try to consume from the window.

        Args:
            key: Rate limit key
            count: Number of requests to record

        Returns:
            RateLimitResult with allowed status and metadata
        """
        now = time.time()

        with self._lock:
            # Initialize or reset expired window
            if key not in self._window_start:
                self._window_start[key] = now
                self._counts[key] = 0
            elif now >= self._window_start[key] + self.window_seconds:
                # Window expired - reset
                self._window_start[key] = now
                self._counts[key] = 0

            window_start = self._window_start[key]
            current_count = self._counts[key]

            if current_count + count <= self.limit:
                # Allowed
                self._counts[key] += count

                return RateLimitResult(
                    allowed=True,
                    remaining=self.limit - self._counts[key],
                    limit=self.limit,
                    reset_at=window_start + self.window_seconds,
                )

            # Rate limited
            return RateLimitResult(
                allowed=False,
                remaining=0,
                limit=self.limit,
                reset_at=window_start + self.window_seconds,
                retry_after=window_start + self.window_seconds - now,
            )

    def reset(self, key: str) -> None:
        """Reset rate limit for a key."""
        with self._lock:
            self._counts.pop(key, None)
            self._window_start.pop(key, None)

    def clear(self) -> None:
        """Clear all rate limit state."""
        with self._lock:
            self._counts.clear()
            self._window_start.clear()


class RateLimitMiddleware:
    """
    Rate limiting middleware for FasterAPI.

    Supports multiple algorithms:
    - token_bucket: Smooth rate limiting with burst support
    - sliding_window: Fixed window with smooth rollover
    - fixed_window: Simple counter per time window

    Usage:
        app.add_middleware(
            RateLimitMiddleware,
            requests_per_minute=100,
            algorithm="token_bucket",
            burst_size=10
        )
    """

    def __init__(
        self,
        app: Any,
        requests_per_minute: int = 60,
        requests_per_second: Optional[float] = None,
        algorithm: Union[str, RateLimitAlgorithm] = RateLimitAlgorithm.TOKEN_BUCKET,
        burst_size: Optional[int] = None,
        window_seconds: Optional[float] = None,
        key_func: Optional[Callable[[Any], str]] = None,
        exclude_paths: Optional[List[str]] = None,
        include_headers: bool = True,
    ):
        """
        Initialize rate limit middleware.

        Args:
            app: ASGI application
            requests_per_minute: Rate limit (requests per minute)
            requests_per_second: Override rate limit (requests per second)
            algorithm: Rate limiting algorithm
            burst_size: Burst capacity for token bucket (default: requests_per_minute)
            window_seconds: Window size for sliding/fixed window (default: 60)
            key_func: Function to extract rate limit key from request scope
            exclude_paths: Paths to exclude from rate limiting
            include_headers: Include X-RateLimit-* headers in response
        """
        self.app = app
        self.include_headers = include_headers
        self.exclude_paths = set(exclude_paths or [])
        self.key_func = key_func or self._default_key_func

        # Calculate rate
        if requests_per_second is not None:
            self.rate = requests_per_second
            window = window_seconds or 1.0
        else:
            self.rate = requests_per_minute / 60.0
            window = window_seconds or 60.0

        # Parse algorithm
        if isinstance(algorithm, str):
            algorithm = RateLimitAlgorithm(algorithm)

        # Create limiter
        if algorithm == RateLimitAlgorithm.TOKEN_BUCKET:
            capacity = burst_size or requests_per_minute
            self._limiter = TokenBucket(rate=self.rate, capacity=capacity)
        elif algorithm == RateLimitAlgorithm.SLIDING_WINDOW:
            limit = int(self.rate * window)
            self._limiter = SlidingWindow(limit=limit, window_seconds=window)
        else:
            limit = int(self.rate * window)
            self._limiter = FixedWindow(limit=limit, window_seconds=window)

    async def __call__(self, scope: Dict, receive: Callable, send: Callable) -> None:
        """ASGI interface."""
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        # Check if path is excluded
        path = scope.get("path", "/")
        if path in self.exclude_paths:
            await self.app(scope, receive, send)
            return

        # Extract key
        key = self.key_func(scope)

        # Check rate limit
        result = self._limiter.consume(key)

        if not result.allowed:
            # Return 429 Too Many Requests
            await self._send_rate_limit_response(send, result)
            return

        if self.include_headers:
            # Wrap send to add rate limit headers
            original_send = send

            async def send_with_headers(message: Dict) -> None:
                if message["type"] == "http.response.start":
                    headers = list(message.get("headers", []))
                    headers.extend(
                        [
                            (b"x-ratelimit-limit", str(result.limit).encode()),
                            (b"x-ratelimit-remaining", str(result.remaining).encode()),
                            (b"x-ratelimit-reset", str(int(result.reset_at)).encode()),
                        ]
                    )
                    message = {**message, "headers": headers}
                await original_send(message)

            await self.app(scope, receive, send_with_headers)
        else:
            await self.app(scope, receive, send)

    async def _send_rate_limit_response(
        self, send: Callable, result: RateLimitResult
    ) -> None:
        """Send 429 Too Many Requests response."""
        import json

        body = json.dumps(
            {
                "detail": "Rate limit exceeded",
                "retry_after": int(result.retry_after) + 1,
            }
        ).encode()

        headers = [
            (b"content-type", b"application/json"),
            (b"content-length", str(len(body)).encode()),
            (b"x-ratelimit-limit", str(result.limit).encode()),
            (b"x-ratelimit-remaining", b"0"),
            (b"x-ratelimit-reset", str(int(result.reset_at)).encode()),
            (b"retry-after", str(int(result.retry_after) + 1).encode()),
        ]

        await send(
            {
                "type": "http.response.start",
                "status": 429,
                "headers": headers,
            }
        )
        await send(
            {
                "type": "http.response.body",
                "body": body,
            }
        )

    @staticmethod
    def _default_key_func(scope: Dict) -> str:
        """
        Default key extractor - uses client IP.

        Checks X-Forwarded-For and X-Real-IP headers for proxied requests.
        """
        headers = dict(scope.get("headers", []))

        # Try X-Forwarded-For
        xff = headers.get(b"x-forwarded-for", b"").decode()
        if xff:
            # Take first IP from comma-separated list
            return xff.split(",")[0].strip()

        # Try X-Real-IP
        xri = headers.get(b"x-real-ip", b"").decode()
        if xri:
            return xri

        # Use client address from scope
        client = scope.get("client")
        if client:
            return client[0]

        return "_default_"
