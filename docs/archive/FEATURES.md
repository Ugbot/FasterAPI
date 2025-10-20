# FasterAPI PostgreSQL Integration - Feature Roadmap

## Overview

This document tracks the features, performance targets, and implementation phases for the high-performance PostgreSQL driver built into FasterAPI.

**Philosophy**: Zero Python on the hot path. All I/O, pooling, protocol, and row decoding done in C++. Python layer is thin and only touches data that needs Python semantics (conversions, validation).

---

## Phase 1: Fast MVP (Current - Stubs)

Goal: Get a working driver with basic features, ready for gradual implementation. All stubs in place; C++ layer will fill in behavior.

### Features to Implement

#### Core Connection Pool
- [ ] Per-core pool sharding (avoid cross-core locks)
  - [ ] Core affinity detection (sched_getaffinity on Linux, sysctl on macOS)
  - [ ] Per-core pool instances with independent LRU prepared statement caches
  - [ ] Fast connection mapping: core_id -> pool shard
- [ ] Connection lifecycle
  - [ ] Health checks with exponential backoff
  - [ ] Idle timeout and connection recycling
  - [ ] Graceful shutdown with connection drainage
- [ ] Prepared statement cache (LRU, per-connection)
  - [ ] Auto-prepare on first use
  - [ ] Cache eviction strategy (LRU + soft limits)
  - [ ] Fallback to unnamed statements on cache miss

#### Query Execution
- [ ] Binary parameter encoding
  - [ ] Fast paths for int, float, bool, text
  - [ ] Zero-allocation parameter array construction
- [ ] Binary result decoding (phase 1 types: int, float, bool, text, bytea, timestamptz, date, numeric, uuid, jsonb)
  - [ ] Zero-copy views into result buffer
  - [ ] Lazy materialization (decode only accessed columns)
- [ ] Query result API
  - [ ] `.all()` - load all rows
  - [ ] `.one()` - assert exactly 1 row
  - [ ] `.first()` - get first row or None
  - [ ] `.scalar()` - first column of first row
  - [ ] `.stream()` - lazy iterator without buffering
  - [ ] `.model(PydanticModel)` - convert to models via pydantic-core
  - [ ] `.into(type[list[tuple]])` - convert to typed containers

#### Transactions
- [ ] Transaction context manager (`with pg.tx()`)
  - [ ] Isolation level support (read_uncommitted, read_committed, repeatable_read, serializable)
  - [ ] Automatic commit on success, rollback on exception
  - [ ] Retry on serialization failures (configurable count)
- [ ] Row-level locking (`FOR UPDATE`, `FOR SHARE`)

#### COPY Operations
- [ ] COPY IN streaming (fast bulk insert)
  - [ ] Backpressure handling
  - [ ] CSV and binary formats
- [ ] COPY OUT streaming (bulk export)
  - [ ] Zero-copy streaming to HTTP response
  - [ ] Configurable buffer sizes

#### Dependency Injection
- [ ] FastAPI `Depends(get_pg)` support
  - [ ] Request-scoped Pg handles via contextvars
  - [ ] Automatic pool.release() on request completion
- [ ] Optional ASGI middleware for request-scoped setup

#### Observability
- [ ] Per-query latency tracking (lock-free histograms)
  - [ ] Percentiles: p50, p95, p99, p99.9
- [ ] Pool metrics (atomic counters)
  - [ ] Active connections, idle, waiters
  - [ ] Total created, recycled, evicted
- [ ] Prometheus-compatible /metrics endpoint (optional integration)

#### Error Handling
- [ ] Exception hierarchy
  - [ ] `PgError` - base exception
  - [ ] `PgConnectionError` - pool exhausted, connection lost
  - [ ] `PgTimeout` - query exceeded deadline
  - [ ] `PgCanceled` - query canceled
  - [ ] `PgIntegrityError` - constraint violations
  - [ ] `PgDataError` - invalid data
- [ ] Error code marshaling from C++ layer

#### Testing & Benchmarks
- [ ] Integration test suite covering all features
  - [ ] Pool lifecycle (create, get, release, close)
  - [ ] Query execution (parameters, results, edge cases)
  - [ ] Transactions and isolation
  - [ ] COPY IN/OUT
  - [ ] Error cases (timeout, disconnect)
- [ ] Performance benchmarks vs psycopg/asyncpg
  - [ ] Connection acquisition latency
  - [ ] Query throughput (queries/sec)
  - [ ] Query latency percentiles
  - [ ] COPY throughput (GB/sec)

### Performance Targets (Phase 1)

| Metric | Target | vs psycopg |
|--------|--------|-----------|
| Connection acquisition | < 100µs | 10x faster |
| Query latency (simple) | < 500µs | 10x faster |
| Query latency (prepared) | < 200µs | 20x faster |
| COPY throughput | > 1 GB/sec | 100x faster |
| Memory per idle connection | < 500 KB | Similar |

### Phase 1 Completion Checklist

- [ ] All C++ stubs have implementations
- [ ] All Python stubs return real data/results
- [ ] Integration tests pass (all non-commented test cases)
- [ ] Benchmarks show 10x improvement over psycopg on simple queries
- [ ] Python types are fully checked (mypy passes)
- [ ] Documentation and examples complete
- [ ] Performance profiling shows <5% GIL contention

---

## Phase 2: Performance & Scale

Goal: Unlock extreme performance through native protocol and advanced pooling.

### Features to Implement

#### Native PostgreSQL Protocol (No libpq)
- [ ] Binary protocol implementation in C++
  - [ ] Message parsing (Bind, Execute, DataRow, etc.)
  - [ ] Asynchronous I/O state machine
  - [ ] Connection startup and authentication
- [ ] Prepared statement management
  - [ ] Named and unnamed statement strategies
  - [ ] Server-side cursor for large result sets
  - [ ] Query plan caching

#### Advanced Types
- [ ] Array types (int[], text[], etc.)
- [ ] Composite types (custom ROW types)
- [ ] Range types (int4range, tstzrange, etc.)
- [ ] Record type streaming

#### Query Pipelining
- [ ] Batch multiple queries in single round-trip
  - [ ] Send N query messages before reading results
  - [ ] Reduces latency for OLTP workloads
  - [ ] Target: 10x throughput improvement for small queries

#### Server-Side Cursors
- [ ] Declare cursor, fetch N rows, close
  - [ ] For result sets larger than memory
  - [ ] Streaming without buffering all rows

#### Advanced Pooling
- [ ] Connection warm-up (pre-execute common queries)
- [ ] Speculative prepared statement preparation (cross-pool)
- [ ] Statement result caching (for safe queries like `SELECT 1`)
- [ ] Per-connection reset strategy (DISCARD vs statement cleanup)
- [ ] Dynamic pool sizing based on load

#### Per-Statement Metrics
- [ ] Query plan execution time breakdown
- [ ] Bytes in/out per statement
- [ ] Lock wait times
- [ ] Server-side processing time vs network latency

#### RLS (Row-Level Security) Profiles
- [ ] Per-user RLS parameter caching
- [ ] Batch RLS setup with prepared statements
- [ ] Automatic SET ROLE on connection acquire

### Performance Targets (Phase 2)

| Metric | Target | vs Phase 1 |
|--------|--------|-----------|
| Query throughput | 1M queries/sec per core | 100x faster |
| Query latency p99 | < 10ms | 50x better |
| COPY throughput | > 5 GB/sec | 5x faster |
| Connection startup | < 50ms | 10x faster |
| Memory per connection | < 300 KB | 40% reduction |

### Phase 2 Completion Checklist

- [ ] Native protocol implementation complete and tested
- [ ] Query pipelining working (10x throughput improvement verified)
- [ ] Array/composite type codecs implemented
- [ ] 1M queries/sec achieved on simple SELECT
- [ ] Benchmarks show consistent sub-10ms p99 latency

---

## Phase 3: Extreme Performance

Goal: Squeeze every last microsecond through JIT compilation, vectorization, and application-level optimizations.

### Features to Implement

#### Compiled Row Projections
- [ ] JIT-compile row decode for known schemas
  - [ ] LLVM codegen at pool init time for common queries
  - [ ] Direct field access without row materialization
  - [ ] SIMD row decoding for parallel column extraction

#### Advanced Codec Optimizations
- [ ] SIMD codecs for bulk operations (AVX2/AVX-512)
  - [ ] Bulk decode 16 rows in parallel
  - [ ] Vectorized byte swapping and type conversion
  - [ ] Target: < 5ns per column

#### Memory-Mapped Result Buffers
- [ ] mmap() result buffers for huge result sets
  - [ ] Avoid copying data from kernel
  - [ ] Page-aligned for hardware prefetching
  - [ ] Transparent to Python layer

#### Query Plan Hints
- [ ] Application-provided query plan hints
  - [ ] Pass index preferences to planner
  - [ ] Prepared statement versioning for plan stability

#### Speculative Compilation
- [ ] Monitor query patterns, pre-compile hot paths
  - [ ] Detect recurring queries across connections
  - [ ] Push prepared statements to new connections before first use

#### Connection Pooling Neural Network (Optional)
- [ ] Predictive pool sizing based on historical patterns
  - [ ] ML model to predict peak load
  - [ ] Auto-scale min/max connections

#### Zero-Copy Row Objects
- [ ] Python objects that view directly into C++ buffers
  - [ ] Row[column] doesn't allocate or copy
  - [ ] GC integration to manage buffer lifetime

### Performance Targets (Phase 3)

| Metric | Target | vs Phase 2 |
|--------|--------|-----------|
| Row decode | < 5ns per column | 10x faster |
| COPY throughput | > 10 GB/sec | 2x faster |
| Compiled query latency | < 100µs | 100x faster |
| Memory footprint | < 100 KB per connection | 3x reduction |

### Phase 3 Completion Checklist

- [ ] SIMD codecs implemented and benchmarked
- [ ] JIT compilation integrated (via LLVM or LLVMLITE)
- [ ] Memory-mapped buffers working for large result sets
- [ ] < 5ns per column decode verified
- [ ] Query latency distribution shows long tail eliminated

---

## Implementation Strategy

### By Phase

1. **Phase 1** (Weeks 1-4): Core infrastructure in place, all interfaces defined. Focus on stability and correctness. C++ fills in stubs gradually.
2. **Phase 2** (Weeks 5-8): Native protocol and pipelining. Major performance jump. Parallel work on codec optimizations.
3. **Phase 3** (Weeks 9+): Extreme optimizations, SIMD, JIT. Diminishing returns; worth doing only for final tuning.

### Development Flow

- Each week: Pick one feature, implement fully (stub -> working -> optimized).
- Run benchmarks after each feature to track progress.
- Keep performance targets visible in CI.
- Maintain backward compatibility in Python API throughout all phases.

---

## Notes for Implementers

- **Stubs first**: All code is stubbed with docstrings explaining expected behavior. Fill in implementations incrementally.
- **Test-driven**: Every feature has integration tests (commented out, to be enabled as implementation proceeds).
- **Performance profiling**: Use `perf`, flamegraph, and benchmarks constantly. If you can't measure it, you can't optimize it.
- **Avoid premature optimization**: Phase 1 should be correct and clean. Only optimize in Phases 2-3 if benchmarks show it matters.
- **Parallel development**: Python and C++ layers can be developed in parallel once interface is defined. Use mock C++ library until real implementation ready.
