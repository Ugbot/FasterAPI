"""
TechEmpower Framework Benchmarks for FasterAPI

Implements the standard TechEmpower test types:
1. JSON Serialization - Return a JSON object
2. Single Database Query - Fetch a single row
3. Multiple Database Queries - Fetch N rows
4. Fortunes - Server-side template rendering
5. Database Updates - Update N rows
6. Plaintext - Return plaintext response

Reference: https://github.com/TechEmpower/FrameworkBenchmarks

These benchmarks measure raw framework performance.
"""

import sys
import asyncio
import random
sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi import App, Depends
from fasterapi.http import Request, Response


# ============================================================================
# TechEmpower Test App
# ============================================================================

app = App(port=8080)


# ============================================================================
# Test 1: JSON Serialization
# ============================================================================

@app.get("/json")
def json_test(req: Request, res: Response):
    """
    Test type 1: JSON serialization
    
    Returns a simple JSON object.
    Tests framework's JSON serialization performance.
    """
    return {"message": "Hello, World!"}


# ============================================================================
# Test 2: Single Database Query
# ============================================================================

# Simulated database
class FakeDB:
    """Simulates database for benchmarking."""
    
    def get_world(self, id: int):
        """Get a single world row."""
        return {
            "id": id,
            "randomNumber": random.randint(1, 10000)
        }
    
    def get_worlds(self, count: int):
        """Get multiple world rows."""
        return [self.get_world(random.randint(1, 10000)) for _ in range(count)]
    
    def update_worlds(self, worlds):
        """Update world rows (simulated)."""
        for world in worlds:
            world["randomNumber"] = random.randint(1, 10000)
        return worlds

db = FakeDB()

def get_db():
    return db


@app.get("/db")
def single_query(req: Request, res: Response, db = Depends(get_db)):
    """
    Test type 2: Single database query
    
    Fetches a single row from database.
    Tests framework's database integration.
    """
    world = db.get_world(random.randint(1, 10000))
    return world


# ============================================================================
# Test 3: Multiple Database Queries
# ============================================================================

@app.get("/queries")
def multiple_queries(req: Request, res: Response, db = Depends(get_db)):
    """
    Test type 3: Multiple database queries
    
    Fetches N rows from database (N from query param, 1-500).
    Tests framework's ability to handle multiple queries.
    """
    # Get query count from URL parameter
    # TechEmpower spec: queries parameter, default 1, min 1, max 500
    queries_param = req.get_query_param("queries")
    
    try:
        count = int(queries_param) if queries_param else 1
    except ValueError:
        count = 1
    
    # Clamp to spec range
    count = max(1, min(500, count))
    
    # Fetch worlds
    worlds = db.get_worlds(count)
    
    return worlds


# ============================================================================
# Test 4: Fortunes (Server-side rendering)
# ============================================================================

@app.get("/fortunes")
def fortunes_test(req: Request, res: Response, db = Depends(get_db)):
    """
    Test type 4: Fortunes
    
    Fetches fortunes from database, adds one, sorts, and renders HTML.
    Tests server-side template rendering.
    """
    # Simulated fortunes
    fortunes = [
        {"id": 1, "message": "fortune: No such file or directory"},
        {"id": 2, "message": "A computer scientist is someone who fixes things that aren't broken."},
        {"id": 3, "message": "After enough decimal places, nobody gives a damn."},
        {"id": 4, "message": "A bad random number generator: 1, 1, 1, 1, 1, 4.33e+67, 1, 1, 1"},
        {"id": 5, "message": "A computer program does what you tell it to do, not what you want it to do."},
    ]
    
    # Add additional fortune
    fortunes.append({"id": 0, "message": "Additional fortune added at request time."})
    
    # Sort by message
    fortunes.sort(key=lambda f: f["message"])
    
    # Render HTML (simplified template)
    html = """<!DOCTYPE html>
<html>
<head><title>Fortunes</title></head>
<body>
<table>
<tr><th>id</th><th>message</th></tr>
"""
    
    for fortune in fortunes:
        # Escape HTML (security requirement)
        message = fortune["message"].replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        html += f"<tr><td>{fortune['id']}</td><td>{message}</td></tr>\n"
    
    html += """</table>
</body>
</html>"""
    
    res.content_type("text/html; charset=utf-8").text(html).send()


# ============================================================================
# Test 5: Database Updates
# ============================================================================

@app.get("/updates")
def updates_test(req: Request, res: Response, db = Depends(get_db)):
    """
    Test type 5: Database updates
    
    Fetches N rows, updates them, and returns.
    Tests framework's database write performance.
    """
    # Get query count
    queries_param = req.get_query_param("queries")
    
    try:
        count = int(queries_param) if queries_param else 1
    except ValueError:
        count = 1
    
    # Clamp to spec range
    count = max(1, min(500, count))
    
    # Fetch worlds
    worlds = db.get_worlds(count)
    
    # Update worlds
    updated = db.update_worlds(worlds)
    
    return updated


# ============================================================================
# Test 6: Plaintext
# ============================================================================

@app.get("/plaintext")
def plaintext_test(req: Request, res: Response):
    """
    Test type 6: Plaintext
    
    Returns simple plaintext response.
    Tests framework's absolute minimum overhead.
    """
    res.content_type("text/plain").text("Hello, World!").send()


# ============================================================================
# Additional Test: Cached Queries
# ============================================================================

@app.get("/cached-worlds")
def cached_worlds(req: Request, res: Response, db = Depends(get_db)):
    """
    Cached queries test (optional).
    
    Tests caching layer performance.
    """
    count = 100  # Fixed count for cache test
    
    # Would check cache first in real implementation
    worlds = db.get_worlds(count)
    
    return worlds


# ============================================================================
# Startup
# ============================================================================

@app.on_event("startup")
def startup():
    print()
    print("╔══════════════════════════════════════════════════════════╗")
    print("║      FasterAPI - TechEmpower Benchmark Server           ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    print("🚀 Running on http://localhost:8080")
    print()
    print("📊 TechEmpower Test Endpoints:")
    print("   GET /json           - JSON serialization")
    print("   GET /db             - Single query")
    print("   GET /queries?queries=N - Multiple queries")
    print("   GET /fortunes       - Server-side rendering")
    print("   GET /updates?queries=N - Database updates")
    print("   GET /plaintext      - Plaintext")
    print()
    print("💡 Run with load testing tool:")
    print("   wrk -t4 -c64 -d30s http://localhost:8080/json")
    print("   ab -n 100000 -c 100 http://localhost:8080/plaintext")
    print()


def main():
    """Run TechEmpower benchmark server."""
    app.run()


if __name__ == "__main__":
    main()

