#!/usr/bin/env python3
"""Quick test to verify query parameter fix"""

import sys
import threading
import time
sys.path.insert(0, "/Users/bengamble/FasterAPI")

from fasterapi import HttpServer

# Create server
server = HttpServer(host="127.0.0.1", port=8090)

# Register a simple handler that prints received arguments
@server.route("GET", "/search")
def search(**kwargs):
    print(f"[Handler] Received kwargs: {kwargs}")
    return f"Query params: q={kwargs.get('q', 'MISSING')}, limit={kwargs.get('limit', 'MISSING')}"

@server.route("GET", "/users/{user_id}/posts")
def get_posts(**kwargs):
    print(f"[Handler] Received kwargs: {kwargs}")
    return f"User {kwargs.get('user_id', 'MISSING')}, page={kwargs.get('page', 'MISSING')}"

print("Starting server on :8090...")
print("Test with:")
print("  curl 'http://127.0.0.1:8090/search?q=test&limit=50'")
print("  curl 'http://127.0.0.1:8090/users/42/posts?page=3'")
print()

server.run()
