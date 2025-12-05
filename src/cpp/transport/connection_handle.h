#pragma once

#include <cstdint>
#include <atomic>
#include <functional>

namespace fasterapi {
namespace transport {

/**
 * Hierarchical 64-bit Connection Handle
 *
 * Encodes transport type, shard (worker thread), epoch, and sequence in a single
 * 64-bit value for efficient routing and stale connection detection.
 *
 * Layout:
 * +--------+--------+--------+----------------------------------+
 * | 63-62  | 61-48  | 47-32  | 31-0                             |
 * | Type   | Shard  | Epoch  | Sequence                         |
 * +--------+--------+--------+----------------------------------+
 *
 * - Type (2 bits): Transport type (WebSocket=0, WebRTC=1, WebTransport=2)
 * - Shard (14 bits): Worker thread ID for lock-free dispatch (up to 16384 workers)
 * - Epoch (16 bits): Restart epoch for stale connection detection
 * - Sequence (32 bits): Per-shard sequence number (4 billion per shard before wrap)
 */
class ConnectionHandle {
public:
    enum class Type : uint8_t {
        WEBSOCKET = 0,
        WEBRTC = 1,
        WEBTRANSPORT = 2,
        RESERVED = 3
    };

    static constexpr uint32_t MAX_SHARDS = 16384;  // 14 bits
    static constexpr uint32_t MAX_EPOCH = 65535;   // 16 bits

    // Invalid handle constant
    static constexpr ConnectionHandle INVALID{0};

    // Default constructor creates invalid handle
    constexpr ConnectionHandle() noexcept : id_(0) {}

    // Construct from raw ID
    static constexpr ConnectionHandle from_raw(uint64_t raw_id) noexcept {
        return ConnectionHandle{raw_id};
    }

    /**
     * Create a new connection handle (thread-safe, lock-free).
     *
     * @param type Transport type
     * @param shard_id Worker thread/shard ID (0-16383)
     * @return New unique connection handle
     */
    static ConnectionHandle create(Type type, uint16_t shard_id) noexcept {
        // Get per-shard sequence counter
        static std::atomic<uint32_t> sequence_counters[MAX_SHARDS] = {};

        // Clamp shard_id
        if (shard_id >= MAX_SHARDS) {
            shard_id = shard_id % MAX_SHARDS;
        }

        // Atomically increment sequence for this shard
        uint32_t seq = sequence_counters[shard_id].fetch_add(1, std::memory_order_relaxed);

        // Get current epoch (static, incremented on server restart)
        uint16_t epoch = get_epoch();

        // Build the handle
        uint64_t id = 0;
        id |= (static_cast<uint64_t>(type) & 0x3) << 62;          // Type: bits 63-62
        id |= (static_cast<uint64_t>(shard_id) & 0x3FFF) << 48;   // Shard: bits 61-48
        id |= (static_cast<uint64_t>(epoch) & 0xFFFF) << 32;      // Epoch: bits 47-32
        id |= seq;                                                  // Sequence: bits 31-0

        return ConnectionHandle{id};
    }

    // Extract components
    Type type() const noexcept {
        return static_cast<Type>((id_ >> 62) & 0x3);
    }

    uint16_t shard_id() const noexcept {
        return static_cast<uint16_t>((id_ >> 48) & 0x3FFF);
    }

    uint16_t epoch() const noexcept {
        return static_cast<uint16_t>((id_ >> 32) & 0xFFFF);
    }

    uint32_t sequence() const noexcept {
        return static_cast<uint32_t>(id_ & 0xFFFFFFFF);
    }

    // Raw ID access
    uint64_t raw() const noexcept { return id_; }

    // Validity check
    bool is_valid() const noexcept { return id_ != 0; }
    explicit operator bool() const noexcept { return is_valid(); }

    // Comparison
    bool operator==(const ConnectionHandle& other) const noexcept {
        return id_ == other.id_;
    }

    bool operator!=(const ConnectionHandle& other) const noexcept {
        return id_ != other.id_;
    }

    bool operator<(const ConnectionHandle& other) const noexcept {
        return id_ < other.id_;
    }

    // Check if handle belongs to current epoch (not stale)
    bool is_current_epoch() const noexcept {
        return epoch() == get_epoch();
    }

    // Hash support for unordered containers
    struct Hash {
        size_t operator()(const ConnectionHandle& h) const noexcept {
            return std::hash<uint64_t>{}(h.id_);
        }
    };

    /**
     * Get current epoch.
     * Epoch starts at 1 and increments on server restart.
     */
    static uint16_t get_epoch() noexcept {
        return current_epoch_.load(std::memory_order_relaxed);
    }

    /**
     * Increment epoch (call on server restart).
     */
    static void increment_epoch() noexcept {
        uint16_t current = current_epoch_.load(std::memory_order_relaxed);
        uint16_t next = (current + 1) & 0xFFFF;
        if (next == 0) next = 1;  // Skip 0 to distinguish from uninitialized
        current_epoch_.store(next, std::memory_order_relaxed);
    }

private:
    uint64_t id_;

    // Private constructor for from_raw()
    explicit constexpr ConnectionHandle(uint64_t id) noexcept : id_(id) {}

    // Current epoch (shared across all instances)
    static inline std::atomic<uint16_t> current_epoch_{1};
};

// Type aliases for convenience
using ConnHandle = ConnectionHandle;
using ConnType = ConnectionHandle::Type;

} // namespace transport
} // namespace fasterapi
