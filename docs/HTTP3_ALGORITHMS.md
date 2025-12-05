# HTTP/3 + QUIC + QPACK Algorithm Documentation

This document outlines the key algorithms implemented in FasterAPI's HTTP/3 stack, based on RFC specifications with Google Quiche as reference.

## Overview

- **QUIC Protocol**: RFC 9000 (transport layer)
- **QUIC Loss Detection**: RFC 9002 (congestion control)
- **HTTP/3**: RFC 9114 (application layer)
- **QPACK**: RFC 9204 (header compression)
- **TLS 1.3**: RFC 8446 (security)

## 1. Variable-Length Integer Encoding (RFC 9000 Section 16)

QUIC uses variable-length integers for efficiency. The first 2 bits encode the length:

```
00 = 1 byte  (0-63)
01 = 2 bytes (0-16383)
10 = 4 bytes (0-1073741823)
11 = 8 bytes (0-4611686018427387903)
```

### Encoding Algorithm

```cpp
size_t encode_varint(uint64_t value, uint8_t* out) {
    if (value <= 63) {
        out[0] = value;
        return 1;
    } else if (value <= 16383) {
        out[0] = 0x40 | (value >> 8);
        out[1] = value & 0xFF;
        return 2;
    } else if (value <= 1073741823) {
        out[0] = 0x80 | (value >> 24);
        out[1] = (value >> 16) & 0xFF;
        out[2] = (value >> 8) & 0xFF;
        out[3] = value & 0xFF;
        return 4;
    } else {
        out[0] = 0xC0 | (value >> 56);
        for (int i = 1; i < 8; i++) {
            out[i] = (value >> (8 * (7 - i))) & 0xFF;
        }
        return 8;
    }
}
```

### Decoding Algorithm

```cpp
int decode_varint(const uint8_t* data, size_t len, uint64_t& out) {
    if (len == 0) return -1;
    
    uint8_t prefix = data[0] >> 6;
    size_t bytes = 1 << prefix;
    
    if (len < bytes) return -1;
    
    out = data[0] & 0x3F;
    for (size_t i = 1; i < bytes; i++) {
        out = (out << 8) | data[i];
    }
    return bytes;
}
```

## 2. QUIC Packet Framing (RFC 9000 Section 17)

### Long Header Format (Initial, 0-RTT, Handshake)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+
|1|1|T T|X X X X|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Version (32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| DCID Len (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               Destination Connection ID (0..160)            ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| SCID Len (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 Source Connection ID (0..160)               ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Short Header Format (1-RTT)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+
|0|1|S|R|R|K|P P|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               Destination Connection ID (0..160)            ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Packet Number (8/16/24/32)              ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Protected Payload (*)                   ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

## 3. Stream Multiplexing (RFC 9000 Section 2.3)

QUIC supports bidirectional and unidirectional streams:

- Stream ID bits:
  - Bit 0: 0=client-initiated, 1=server-initiated
  - Bit 1: 0=bidirectional, 1=unidirectional

### Stream State Machine

```
       o
       | Create Stream (allocate stream ID)
       | Peer Creates Bidirectional Stream
       v
   +-------+
   | Ready |
   +-------+
       |
       | Send STREAM / Receive STREAM
       v
   +-------+
   | Send  |
   +-------+
       |
       | Send FIN
       v
   +-------+
   | Data  |
   | Sent  |
   +-------+
       |
       | Receive FIN
       v
   +-------+
   | Data  |
   | Recvd |
   +-------+
```

## 4. Flow Control (RFC 9000 Section 4)

### Per-Stream Flow Control

```cpp
struct StreamFlowControl {
    uint64_t max_data;        // Max offset we can send
    uint64_t sent_offset;     // Offset we've sent up to
    uint64_t recv_max_data;   // Max offset peer can send
    uint64_t recv_offset;     // Offset we've received up to
    
    bool can_send(size_t bytes) {
        return sent_offset + bytes <= max_data;
    }
    
    void update_window(uint64_t new_max) {
        max_data = new_max;
    }
};
```

### Connection-Level Flow Control

```cpp
struct ConnectionFlowControl {
    uint64_t max_data;           // Total connection limit
    uint64_t total_sent;         // Total sent across all streams
    
    bool can_send(size_t bytes) {
        return total_sent + bytes <= max_data;
    }
};
```

## 5. Loss Detection and Recovery (RFC 9002)

### ACK Processing

QUIC uses ACK ranges for efficiency:

```cpp
struct AckRange {
    uint64_t first;  // First acknowledged packet
    uint64_t last;   // Last acknowledged packet
};

void process_ack(const AckFrame& ack) {
    for (const auto& range : ack.ranges) {
        for (uint64_t pkt = range.first; pkt <= range.last; pkt++) {
            mark_packet_acked(pkt);
        }
    }
    
    detect_lost_packets();
}
```

### Loss Detection Algorithm

```cpp
void detect_lost_packets() {
    uint64_t loss_delay = max(kTimeThreshold * max_rtt, kGranularity);
    uint64_t lost_send_time = now() - loss_delay;
    
    for (auto& [pkt_num, sent_pkt] : sent_packets) {
        if (sent_pkt.time_sent <= lost_send_time ||
            largest_acked >= pkt_num + kPacketThreshold) {
            declare_lost(pkt_num);
        }
    }
}
```

## 6. NewReno Congestion Control (RFC 9002 Section 7.3)

### Slow Start

```cpp
void on_ack_received(size_t acked_bytes) {
    if (congestion_window < ssthresh) {
        // Slow start: exponential growth
        congestion_window += acked_bytes;
    } else {
        // Congestion avoidance: linear growth
        congestion_window += max_datagram_size * acked_bytes / congestion_window;
    }
}
```

### Congestion Event

```cpp
void on_congestion_event() {
    ssthresh = congestion_window * kLossReductionFactor;  // 0.5
    congestion_window = max(ssthresh, kMinimumWindow);
    recovery_start_time = now();
}
```

## 7. QPACK Static Table (RFC 9204 Appendix A)

QPACK defines 99 static table entries for common HTTP headers:

```cpp
static const QPACKStaticEntry static_table[] = {
    {0, ":authority", ""},
    {1, ":path", "/"},
    {2, "age", "0"},
    {3, "content-disposition", ""},
    {4, "content-length", "0"},
    {5, "cookie", ""},
    // ... 94 more entries
};
```

## 8. QPACK Dynamic Table (RFC 9204 Section 3.2)

### Ring Buffer Implementation

```cpp
class QPACKDynamicTable {
    struct Entry {
        std::string name;
        std::string value;
        size_t size;  // name.length() + value.length() + 32
    };
    
    std::vector<Entry> entries;
    size_t capacity;
    size_t size;
    size_t insert_index;
    size_t drop_index;
    
    void insert(const std::string& name, const std::string& value) {
        size_t entry_size = name.length() + value.length() + 32;
        
        // Evict entries if needed
        while (size + entry_size > capacity && !entries.empty()) {
            evict_oldest();
        }
        
        entries.push_back({name, value, entry_size});
        size += entry_size;
        insert_index++;
    }
    
    void evict_oldest() {
        size -= entries.front().size;
        entries.erase(entries.begin());
        drop_index++;
    }
};
```

## 9. QPACK Encoder (RFC 9204 Section 4)

### Indexed Header Field

```
  0   1   2   3   4   5   6   7
+---+---+---+---+---+---+---+---+
| 1 | T |      Index (6+)       |
+---+---+-----------------------+
```

- T=1: Static table
- T=0: Dynamic table

### Literal with Name Reference

```
  0   1   2   3   4   5   6   7
+---+---+---+---+---+---+---+---+
| 0 | 1 | N | T | Name Index (4+) |
+---+---+---+---+-----------------+
| H |     Value Length (7+)     |
+---+---------------------------+
|  Value String (Length bytes)  |
+-------------------------------+
```

### Huffman Coding

QPACK uses the same Huffman table as HPACK (RFC 7541 Appendix B):

```cpp
size_t huffman_encode(const uint8_t* input, size_t len, uint8_t* output) {
    size_t bit_pos = 0;
    
    for (size_t i = 0; i < len; i++) {
        const HuffmanCode& code = huffman_table[input[i]];
        write_bits(output, bit_pos, code.bits, code.length);
        bit_pos += code.length;
    }
    
    // Pad with 1s
    if (bit_pos % 8 != 0) {
        write_bits(output, bit_pos, 0xFF, 8 - (bit_pos % 8));
    }
    
    return (bit_pos + 7) / 8;
}
```

## 10. HTTP/3 Frame Types (RFC 9114 Section 7.2)

```cpp
enum FrameType : uint64_t {
    DATA = 0x00,
    HEADERS = 0x01,
    CANCEL_PUSH = 0x03,
    SETTINGS = 0x04,
    PUSH_PROMISE = 0x05,
    GOAWAY = 0x07,
    MAX_PUSH_ID = 0x0D,
};

struct Frame {
    uint64_t type;
    uint64_t length;
    uint8_t* payload;
};
```

## 11. HTTP/3 Request/Response Flow

```
Client                                            Server
  |                                                  |
  |-- HEADERS (method, path, headers) ------------->|
  |                                                  |
  |-- DATA (request body, optional) --------------->|
  |                                                  |
  |<--------------- HEADERS (status, headers) ------|
  |                                                  |
  |<--------------- DATA (response body) -----------|
  |                                                  |
```

## 12. Server Push (RFC 9114 Section 4.6)

```
Server
  |
  |-- PUSH_PROMISE (promised stream ID, headers) -->|
  |                                                  |
  |-- HEADERS (status, headers on push stream) ---->|
  |                                                  |
  |-- DATA (push response body) ------------------->|
```

## Performance Optimizations

### 1. Object Pooling

Pre-allocate packets, streams, and buffers:

```cpp
ObjectPool<QUICPacket> packet_pool(1000);
ObjectPool<QUICStream> stream_pool(100);
```

### 2. Ring Buffers

Use ring buffers for send/receive data:

```cpp
RingBuffer send_buffer(1024 * 1024);  // 1MB
RingBuffer recv_buffer(1024 * 1024);
```

### 3. Zero-Copy

Use `std::string_view` for header parsing to avoid allocations.

### 4. Vectorization

Align data structures and use SIMD for packet processing where possible.

## References

1. RFC 9000: QUIC Transport Protocol
2. RFC 9001: Using TLS to Secure QUIC
3. RFC 9002: QUIC Loss Detection and Congestion Control
4. RFC 9114: HTTP/3
5. RFC 9204: QPACK: Field Compression for HTTP/3
6. Google Quiche: https://github.com/google/quiche

## Implementation Checklist

- [ ] Variable-length integer codec
- [ ] QUIC packet framing (long/short headers)
- [ ] Connection ID management
- [ ] Stream multiplexing
- [ ] Flow control (per-stream and connection)
- [ ] Loss detection and ACK processing
- [ ] NewReno congestion control
- [ ] QPACK static table (99 entries)
- [ ] QPACK dynamic table (ring buffer)
- [ ] QPACK encoder/decoder
- [ ] Huffman coding
- [ ] HTTP/3 frame parsing
- [ ] TLS 1.3 integration
- [ ] UDP socket management
- [ ] Event loop integration (kqueue/epoll)
