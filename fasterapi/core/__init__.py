"""
FasterAPI Core Async Utilities

Provides high-performance async/await support with Seastar-style futures.
"""

from .future import Future, when_all, when_any
from .reactor import Reactor

__all__ = [
    'Future',
    'when_all',
    'when_any',
    'Reactor',
]

