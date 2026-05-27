> **Research note — snapshot, not current state.**
> This document is from FasterAPI's exploration phase. Claims here
> reflect what was being investigated at the time, not the testbed's
> current state. FasterAPI is an experimental testbed, not a framework.
> Ping [@ugbot](https://github.com/ugbot) for the actual framework
> built on top of this toolkit.
# HTTP/3 Implementation Gap Analysis

> Last updated: 2026-01-01

Analysis of the FasterAPI HTTP/3 implementation to identify what's missing or incomplete for production readiness.

## Implementation Status Summary

| Component | Status | Coverage |
|-----------|--------|----------|
| **HTTP/3 Core** | ~70% | Frames, parser, connection handler |
| **QUIC Transport** | ~85% | Full RFC 9000 core features |
| **QPACK** | ~90% | Encoding complete, decoding partial |
| **TLS 1.3 Integration** | Complete | Via quictls |
| **Flow Control** | Complete | Connection + stream level |
| **Congestion Control** | Complete | NewReno algorithm |
| **WebTransport** | ~60% | Basic integration, incomplete |

---

## Critical Gaps (P0 - Must Fix for Production)

### 1. Stream Reassembly - NOT IMPLEMENTED

**Location**: `src/cpp/http/quic/quic_stream.h:155`

```cpp
// TODO: Implement reassembly buffer
return -1;  // Simplified: reject out-of-order for now
```

**Impact**: Out-of-order QUIC packets cause stream failures. Only works when packets arrive in-order.

**Fix Required**: Implement ring buffer-based reassembly with gap tracking.

### 2. Huffman Decoding - NOT IMPLEMENTED

**Location**: `src/cpp/http/http3/qpack.h:496`, `src/cpp/http/qpack/qpack_decoder.h:116`

```cpp
// Huffman decoding not implemented - treat as raw
// Simplified: not implemented
```

**Impact**: Headers encoded with Huffman by peers are misinterpreted.

**Fix Required**: Implement RFC 7541 Huffman decoding table lookup.

### 3. Packet Decryption TODO

**Location**: `src/cpp/http/quic/quic_connection.cpp:278`

```cpp
// TODO: decrypt when crypto is implemented
```

**Impact**: Currently processing unencrypted/test packets only.

**Status**: TLS infrastructure exists (`quic_tls.h`) but integration incomplete.

---

## Partial Implementations

### 4. Connection Migration
- PATH_CHALLENGE/PATH_RESPONSE frames defined
- Connection ID rotation NOT implemented
- Address validation token NOT implemented
- NAT rebinding detection missing

### 5. 0-RTT Support
- Zero-RTT packet type defined
- TLS 0-RTT secret handling exists
- Resumption and anti-replay NOT implemented

### 6. WebTransport
- WEBTRANSPORT_STREAM frame type defined (0x41)
- CONNECT + :protocol=webtransport conversion works
- Missing: Proper bidirectional message framing, session management

### 7. Key Update
- Framework exists
- KEY_UPDATE frame handling incomplete

---

## Missing Features (RFC Compliance)

| Feature | RFC | Status | Priority |
|---------|-----|--------|----------|
| Stream Reassembly | 9000 | Missing | **P0** |
| Huffman Decoding | 7541 | Missing | **P0** |
| Connection Migration | 9000 | Partial | P1 |
| Stateless Reset | 9000 | Missing | P1 |
| 0-RTT Resumption | 9000 | Partial | P2 |
| ECN Support | 9000 | Partial | P2 |
| Version Negotiation | 9000 | Basic | P2 |
| Preferred Address | 9000 | Missing | P3 |

---

## Existing Strengths

### Fully Implemented
- QUIC packet parsing/serialization (all types)
- Connection state machine (IDLE→HANDSHAKE→ESTABLISHED→CLOSING→DRAINING→CLOSED)
- Stream multiplexing (bidi + uni)
- Flow control (connection + stream level)
- Congestion control (NewReno with pacing)
- ACK tracking and loss detection (RFC 9002)
- TLS 1.3 via quictls
- QPACK encoding with static/dynamic tables
- 26 QUIC frame types
- VarInt encoding (RFC 9000 Section 16)

### Test Coverage
- **Strong**: VarInt, QPACK, flow control, congestion, ACK tracker, frames
- **Moderate**: Integration tests, WebTransport E2E
- **Weak**: Stream lifecycle, packet handling, handshake flow

---

## Key Files

```
HTTP/3 Core:
├── src/cpp/http/h3_handler.h|cpp           # High-level API
├── src/cpp/http/http3_connection.h|cpp     # Connection management
├── src/cpp/http/http3_parser.h|cpp         # Frame parsing
└── src/cpp/http/http3/http3_frames.h       # Frame definitions

QUIC Transport:
├── src/cpp/http/quic/quic_connection.h|cpp # Connection state
├── src/cpp/http/quic/quic_stream.h|cpp     # Stream handling (NEEDS REASSEMBLY)
├── src/cpp/http/quic/quic_packet.h|cpp     # Packet format
├── src/cpp/http/quic/quic_tls.h            # TLS 1.3 integration
├── src/cpp/http/quic/quic_flow_control.h   # Window management
├── src/cpp/http/quic/quic_congestion.h|cpp # NewReno algorithm
└── src/cpp/http/quic/quic_ack_tracker.h    # ACK/loss detection

QPACK:
├── src/cpp/http/qpack/qpack_encoder.h|cpp  # Encoding (complete)
├── src/cpp/http/qpack/qpack_decoder.h|cpp  # Decoding (NEEDS HUFFMAN)
├── src/cpp/http/qpack/qpack_static_table.h # 99 static entries
└── src/cpp/http/qpack/qpack_dynamic_table.h # Dynamic table

Tests:
├── tests/gtest/gtest_http3_integration.cpp
├── tests/gtest/gtest_quic_*.cpp (7 files)
├── tests/gtest/gtest_qpack.cpp
└── benchmarks/fasterapi/bench_http3.cpp
```

---

## Recommended Priority Order

### Phase 1: Core Functionality (P0)

1. **Implement stream reassembly** in `quic_stream.cpp`
   - Add reassembly buffer (ring buffer with gap tracking)
   - Handle out-of-order STREAM frames
   - Deliver to application in-order

2. **Implement Huffman decoding** in `qpack_decoder.cpp`
   - Add RFC 7541 Huffman decode table
   - Integrate with existing decoder

### Phase 2: Security & Reliability (P1)

3. **Complete packet encryption integration**
   - Wire up quic_tls.h with quic_connection.cpp
   - Implement encrypt/decrypt in packet handling

4. **Implement stateless reset**
   - Generate stateless reset tokens
   - Handle received resets

5. **Connection migration**
   - Implement connection ID rotation
   - Handle PATH_CHALLENGE/PATH_RESPONSE properly

### Phase 3: Performance & Features (P2)

6. **0-RTT resumption**
7. **ECN support**
8. **Complete WebTransport integration**

---

## Production Readiness Assessment

**Current State**: Development/Testing only

**Blockers for Production**:
1. No encryption in practice (TLS code exists but not wired up)
2. Out-of-order packets fail
3. Huffman-encoded headers from peers break
