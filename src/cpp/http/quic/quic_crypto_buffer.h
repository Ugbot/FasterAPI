#pragma once

/**
 * @file quic_crypto_buffer.h
 * @brief QUIC CRYPTO frame buffer for TLS handshake data (RFC 9001)
 *
 * Buffers and reassembles TLS handshake data from CRYPTO frames at each
 * encryption level. Handles out-of-order delivery and duplicate detection.
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>

namespace fasterapi {
namespace quic {

/**
 * QUIC encryption levels (RFC 9001 Section 4.1).
 */
enum class EncryptionLevel : uint8_t {
    INITIAL = 0,      // Initial keys (derived from DCID)
    ZERO_RTT = 1,     // 0-RTT keys (early data, client->server only)
    HANDSHAKE = 2,    // Handshake keys
    ONE_RTT = 3,      // 1-RTT keys (application data)
    NUM_LEVELS = 4
};

/**
 * Convert encryption level to string for debugging.
 */
inline const char* encryption_level_name(EncryptionLevel level) noexcept {
    switch (level) {
        case EncryptionLevel::INITIAL: return "Initial";
        case EncryptionLevel::ZERO_RTT: return "0-RTT";
        case EncryptionLevel::HANDSHAKE: return "Handshake";
        case EncryptionLevel::ONE_RTT: return "1-RTT";
        default: return "Unknown";
    }
}

/**
 * CRYPTO frame buffer for reassembling TLS handshake data.
 *
 * TLS handshake messages may span multiple CRYPTO frames and arrive
 * out of order. This buffer reassembles them in the correct order.
 *
 * Features:
 * - Out-of-order reassembly with gap tracking
 * - Duplicate detection
 * - Separate read and write cursors
 * - Fixed-size buffer (no dynamic allocation in hot path)
 */
class CryptoBuffer {
public:
    static constexpr size_t kMaxBufferSize = 64 * 1024;  // 64KB max TLS handshake

    CryptoBuffer() noexcept
        : read_offset_(0),
          write_offset_(0),
          recv_offset_(0),
          data_size_(0) {
        std::memset(buffer_, 0, kMaxBufferSize);
    }

    /**
     * Receive CRYPTO frame data.
     *
     * @param offset Offset in the crypto stream
     * @param data Frame data
     * @param length Data length
     * @return 0 on success, -1 if buffer full, -2 if invalid offset
     */
    int receive_data(uint64_t offset, const uint8_t* data, size_t length) noexcept {
        if (length == 0) return 0;

        // Check if data fits in buffer
        uint64_t end_offset = offset + length;
        if (end_offset > kMaxBufferSize) {
            return -1;  // Would overflow buffer
        }

        // Check for duplicate/old data
        if (end_offset <= recv_offset_) {
            return 0;  // Already received, ignore
        }

        // Handle overlapping data (partial duplicate)
        size_t copy_offset = 0;
        if (offset < recv_offset_) {
            copy_offset = recv_offset_ - offset;
            offset = recv_offset_;
            length -= copy_offset;
        }

        // Copy data to buffer
        std::memcpy(buffer_ + offset, data + copy_offset, length);

        // Update gap tracking
        if (offset == recv_offset_) {
            // Contiguous data - advance recv_offset
            recv_offset_ = end_offset;

            // Check if any gaps are now filled
            while (!gaps_.empty()) {
                auto& gap = gaps_.front();
                if (gap.start <= recv_offset_) {
                    if (gap.end > recv_offset_) {
                        recv_offset_ = gap.end;
                    }
                    gaps_.erase(gaps_.begin());
                } else {
                    break;
                }
            }
        } else {
            // Out of order - record gap
            insert_gap(recv_offset_, offset);
            // Update recv_offset if this extends contiguous data
            if (recv_offset_ < end_offset) {
                recv_offset_ = end_offset;
            }
        }

        data_size_ = std::max(data_size_, static_cast<size_t>(recv_offset_));
        return 0;
    }

    /**
     * Read contiguous data from the buffer.
     *
     * @param out Output buffer
     * @param max_len Maximum bytes to read
     * @return Number of bytes read
     */
    size_t read(uint8_t* out, size_t max_len) noexcept {
        size_t available = contiguous_available();
        size_t to_read = std::min(available, max_len);

        if (to_read > 0) {
            std::memcpy(out, buffer_ + read_offset_, to_read);
            read_offset_ += to_read;
        }

        return to_read;
    }

    /**
     * Peek at data without consuming it.
     *
     * @param out Output buffer
     * @param max_len Maximum bytes to peek
     * @return Number of bytes copied
     */
    size_t peek(uint8_t* out, size_t max_len) const noexcept {
        size_t available = contiguous_available();
        size_t to_read = std::min(available, max_len);

        if (to_read > 0) {
            std::memcpy(out, buffer_ + read_offset_, to_read);
        }

        return to_read;
    }

    /**
     * Write data for sending (at write offset).
     *
     * @param data Data to write
     * @param length Data length
     * @return Number of bytes written, -1 if buffer full
     */
    ssize_t write(const uint8_t* data, size_t length) noexcept {
        if (write_offset_ + length > kMaxBufferSize) {
            return -1;  // Buffer full
        }

        std::memcpy(send_buffer_ + write_offset_, data, length);
        write_offset_ += length;
        return static_cast<ssize_t>(length);
    }

    /**
     * Get pending send data.
     *
     * @param offset Starting offset for this send
     * @param max_len Maximum bytes to get
     * @return Pair of (data pointer, length)
     */
    std::pair<const uint8_t*, size_t> get_send_data(uint64_t offset,
                                                     size_t max_len) const noexcept {
        if (offset >= write_offset_) {
            return {nullptr, 0};
        }

        size_t available = write_offset_ - offset;
        size_t length = std::min(available, max_len);
        return {send_buffer_ + offset, length};
    }

    /**
     * Get amount of contiguous data available to read.
     */
    size_t contiguous_available() const noexcept {
        // Only return data up to first gap
        if (gaps_.empty()) {
            return recv_offset_ - read_offset_;
        }

        // Data available up to first gap
        size_t first_gap = gaps_.front().start;
        if (first_gap <= read_offset_) {
            return 0;  // At a gap
        }
        return first_gap - read_offset_;
    }

    /**
     * Get total data available (may include gaps).
     */
    size_t total_available() const noexcept {
        return data_size_ > read_offset_ ? data_size_ - read_offset_ : 0;
    }

    /**
     * Get current read offset.
     */
    uint64_t read_offset() const noexcept { return read_offset_; }

    /**
     * Get current write offset (for sending).
     */
    uint64_t write_offset() const noexcept { return write_offset_; }

    /**
     * Get highest received offset.
     */
    uint64_t recv_offset() const noexcept { return recv_offset_; }

    /**
     * Check if buffer has any gaps.
     */
    bool has_gaps() const noexcept { return !gaps_.empty(); }

    /**
     * Reset buffer to initial state.
     */
    void reset() noexcept {
        read_offset_ = 0;
        write_offset_ = 0;
        recv_offset_ = 0;
        data_size_ = 0;
        gaps_.clear();
    }

    /**
     * Check if there's pending data to send.
     */
    bool has_pending_send() const noexcept {
        return send_offset_ < write_offset_;
    }

    /**
     * Get next send offset (for retransmission tracking).
     */
    uint64_t next_send_offset() const noexcept {
        return send_offset_;
    }

    /**
     * Advance send offset after successful send.
     */
    void advance_send_offset(size_t bytes) noexcept {
        send_offset_ += bytes;
    }

private:
    struct Gap {
        uint64_t start;
        uint64_t end;
    };

    void insert_gap(uint64_t start, uint64_t end) noexcept {
        if (start >= end) return;

        Gap new_gap{start, end};

        // Find insertion point and merge with adjacent gaps
        auto it = std::lower_bound(gaps_.begin(), gaps_.end(), new_gap,
            [](const Gap& a, const Gap& b) { return a.start < b.start; });

        // Merge with previous if overlapping
        if (it != gaps_.begin()) {
            auto prev = it - 1;
            if (prev->end >= start) {
                prev->end = std::max(prev->end, end);
                // Check if we can merge with next too
                while (it != gaps_.end() && prev->end >= it->start) {
                    prev->end = std::max(prev->end, it->end);
                    it = gaps_.erase(it);
                }
                return;
            }
        }

        // Merge with next if overlapping
        if (it != gaps_.end() && end >= it->start) {
            it->start = start;
            it->end = std::max(it->end, end);
            return;
        }

        gaps_.insert(it, new_gap);
    }

    // Receive buffer (for incoming CRYPTO data)
    uint8_t buffer_[kMaxBufferSize];
    uint64_t read_offset_;     // Next byte to read
    uint64_t recv_offset_;     // Highest contiguous byte received
    size_t data_size_;         // Total data size received
    std::vector<Gap> gaps_;    // Gaps in received data

    // Send buffer (for outgoing CRYPTO data)
    uint8_t send_buffer_[kMaxBufferSize];
    uint64_t write_offset_;    // Next byte to write
    uint64_t send_offset_{0};  // Next byte to send
};

/**
 * Per-encryption-level CRYPTO state.
 */
struct CryptoState {
    CryptoBuffer buffer;
    bool keys_available{false};
    bool handshake_complete{false};
};

} // namespace quic
} // namespace fasterapi
