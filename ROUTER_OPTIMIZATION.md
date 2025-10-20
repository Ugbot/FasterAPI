# High-Performance Router Implementation

## Current State 🔴

The current routing implementation uses simple hash map lookups:

```cpp
// Current: O(1) for exact match, but NO path parameters
std::unordered_map<std::string, std::unordered_map<std::string, RouteHandler>> routes_;

// Lookup:
auto method_routes = routes_.find(method);
auto route_handler = method_routes->second.find(path);  // Exact match only!
```

**Problems:**
- ❌ No support for path parameters (e.g., `/user/{id}`)
- ❌ No wildcard matching
- ❌ Must do exact string match
- ❌ No route priorities
- ❌ Linear search for parameterized routes

## Proposed Solution ✅

Implement a **Radix Tree Router** (like Gin, httprouter, chi):

### Features
- ✅ Path parameter extraction: `/user/{id}` matches `/user/123`
- ✅ Wildcard routes: `/files/*filepath`
- ✅ Priority-based matching (static > param > wildcard)
- ✅ O(k) lookup where k = path length
- ✅ Zero allocations for match
- ✅ Thread-safe for concurrent reads

### Performance Targets

| Operation | Current | Target | Improvement |
|-----------|---------|--------|-------------|
| Static route | ~100ns | ~50ns | 2x faster |
| Param route | N/A | ~200ns | New feature |
| Build time | O(n) | O(n) | Same |
| Memory | O(n) | O(n×k) | Acceptable |

## Implementation Plan

### Phase 1: Radix Tree Core ✅
- Node structure with path segments
- Insert operation for route registration
- Match operation for request routing
- Parameter extraction

### Phase 2: Integration ✅
- Replace `std::unordered_map` in HttpServer
- Update `add_route()` to use radix tree
- Update `handle_request()` to extract params
- Add benchmarks

### Phase 3: Advanced Features 🔄
- Route groups with common prefixes
- Middleware per-route
- Route introspection API
- OpenAPI schema generation

## Next Steps

Run:
```bash
# View implementation
cat src/cpp/http/router.h
cat src/cpp/http/router.cpp

# Run benchmarks
python3 benchmarks/bench_router.py

# Run tests
python3 tests/test_router.py
```

