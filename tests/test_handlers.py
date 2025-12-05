#!/usr/bin/env python3.13
"""
Shared Test Handlers Module
Importable handlers for E2E tests - enables worker process execution for true parallelism
"""

import random
import time


def health_handler():
    """Simple health check handler with randomized data"""
    return {"status": "healthy", "api": "cpp", "random": random.randint(1000, 9999)}


def random_handler():
    """Handler returning random data to verify no caching"""
    return {
        "random": random.randint(1000, 9999),
        "timestamp": time.time()
    }


def post_handler():
    """POST endpoint handler"""
    return {
        "created": True,
        "method": "POST"
    }


def put_handler():
    """PUT endpoint handler"""
    return {
        "updated": True,
        "method": "PUT"
    }


def delete_handler():
    """DELETE endpoint handler (returns empty dict)"""
    return {}


def get_item(item_id: int):
    """Handler with path parameter"""
    return {"item_id": item_id, "name": f"Item {item_id}"}


def search_items(q: str = "", limit: int = 10):
    """Handler with query parameters and defaults"""
    return {
        "query": q,
        "limit": limit,
        "results": [f"result{i}" for i in range(min(limit, 3))]
    }


def health_check():
    """Simple handler with no parameters"""
    return {"status": "healthy", "message": "Server is running"}
