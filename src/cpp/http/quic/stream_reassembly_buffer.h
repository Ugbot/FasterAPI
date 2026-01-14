#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

namespace fasterapi {
namespace quic {

/**
 * Stream Reassembly Buffer for QUIC out-of-order packet handling.
 *
 * Design principles (per CLAUDE.md):
 * - No allocations in hot path
 * - Fixed-size gap array (32 entries max)
 * - Cache-line aligned for performance
 * - Zero-copy delivery to application
 *
 * Handles:
 * - In-order data (fast path)
 * - Out-of-order data with gap tracking
 * - Duplicate detection and handling
 * - Overlapping segment handling
 * - FIN position tracking
 *
 * RFC 9000 Section 2.2: Stream data is delivered to the application in order.
 */

/**
 * Represents a contiguous segment of received data.
 */
struct StreamSegment {
    uint64_t offset;    // Start offset in stream
    uint64_t length;    // Length of segment

    uint64_t end() const noexcept { return offset + length; }

    bool overlaps(const StreamSegment& other) const noexcept {
        return offset < other.end() && other.offset < end();
    }

    bool adjacent_to(const StreamSegment& other) const noexcept {
        return end() == other.offset || other.end() == offset;
    }
};

/**
 * Pre-allocated reassembly buffer for out-of-order STREAM frames.
 */
class StreamReassemblyBuffer {
public:
    static constexpr size_t MAX_SEGMENTS = 32;
    static constexpr size_t BUFFER_SIZE = 64 * 1024;  // 64KB

    StreamReassemblyBuffer() noexcept
        : segment_count_(0),
          bytes_consumed_(0),
          largest_received_(0),
          fin_offset_(UINT64_MAX),
          fin_received_(false) {
        std::memset(buffer_, 0, BUFFER_SIZE);
        std::memset(segments_, 0, sizeof(segments_));
    }

    /**
     * Write data at specified stream offset.
     *
     * Handles:
     * - In-order data (extends first segment)
     * - Out-of-order data (creates new segment)
     * - Overlapping data (merges segments)
     * - Duplicate data (ignored)
     *
     * @param offset Stream byte offset
     * @param data Data pointer
     * @param length Data length
     * @return Bytes written, or -1 on error (gap limit, capacity)
     */
    ssize_t write_at(uint64_t offset, const uint8_t* data, size_t length) noexcept {
        if (length == 0) return 0;

        // Check if this would exceed buffer capacity
        // We use a sliding window: buffer holds [bytes_consumed_, bytes_consumed_ + BUFFER_SIZE)
        if (offset + length > bytes_consumed_ + BUFFER_SIZE) {
            // Would overflow buffer - caller should apply flow control
            return -1;
        }

        // Ignore data before our consumption point (already delivered)
        if (offset + length <= bytes_consumed_) {
            return 0;  // Complete duplicate
        }

        // Adjust for partial overlap with already-consumed data
        if (offset < bytes_consumed_) {
            size_t skip = static_cast<size_t>(bytes_consumed_ - offset);
            offset = bytes_consumed_;
            data += skip;
            length -= skip;
        }

        // Copy data to buffer (circular addressing)
        size_t buf_offset = static_cast<size_t>(offset % BUFFER_SIZE);
        size_t first_chunk = std::min(length, BUFFER_SIZE - buf_offset);
        std::memcpy(buffer_ + buf_offset, data, first_chunk);
        if (length > first_chunk) {
            std::memcpy(buffer_, data + first_chunk, length - first_chunk);
        }

        // Update largest received
        if (offset + length > largest_received_) {
            largest_received_ = offset + length;
        }

        // Insert segment into tracking array
        if (!insert_segment(offset, length)) {
            return -1;  // Too many gaps
        }

        return static_cast<ssize_t>(length);
    }

    /**
     * Read contiguous data from the beginning of the stream.
     *
     * Only returns data that can be delivered in-order.
     *
     * @param data Output buffer
     * @param max_length Maximum bytes to read
     * @return Bytes read (only contiguous from start)
     */
    ssize_t read(uint8_t* data, size_t max_length) noexcept {
        size_t available = contiguous_available();
        if (available == 0) return 0;

        size_t to_read = std::min(available, max_length);

        // Read from circular buffer
        size_t buf_offset = static_cast<size_t>(bytes_consumed_ % BUFFER_SIZE);
        size_t first_chunk = std::min(to_read, BUFFER_SIZE - buf_offset);
        std::memcpy(data, buffer_ + buf_offset, first_chunk);
        if (to_read > first_chunk) {
            std::memcpy(data + first_chunk, buffer_, to_read - first_chunk);
        }

        // Advance consumption point
        bytes_consumed_ += to_read;

        // Update first segment
        if (segment_count_ > 0) {
            if (bytes_consumed_ >= segments_[0].end()) {
                // Remove first segment entirely
                for (size_t i = 0; i < segment_count_ - 1; i++) {
                    segments_[i] = segments_[i + 1];
                }
                segment_count_--;
            } else if (bytes_consumed_ > segments_[0].offset) {
                // Shrink first segment
                segments_[0].length -= static_cast<size_t>(bytes_consumed_ - segments_[0].offset);
                segments_[0].offset = bytes_consumed_;
            }
        }

        return static_cast<ssize_t>(to_read);
    }

    /**
     * Get amount of contiguous data available from the start.
     */
    size_t contiguous_available() const noexcept {
        if (segment_count_ == 0) return 0;

        // First segment must start at our consumption point
        if (segments_[0].offset > bytes_consumed_) {
            return 0;  // Gap at the beginning
        }

        return static_cast<size_t>(segments_[0].end() - bytes_consumed_);
    }

    /**
     * Get next expected offset.
     */
    uint64_t next_expected_offset() const noexcept {
        return bytes_consumed_;
    }

    /**
     * Set FIN offset.
     */
    void set_fin_offset(uint64_t offset) noexcept {
        fin_offset_ = offset;
        fin_received_ = true;
    }

    /**
     * Check if stream is complete (all data received up to FIN).
     */
    bool is_complete() const noexcept {
        if (!fin_received_) return false;
        return bytes_consumed_ >= fin_offset_;
    }

    /**
     * Check if FIN has been received.
     */
    bool fin_received() const noexcept {
        return fin_received_;
    }

    /**
     * Get FIN offset (-1 if not set).
     */
    uint64_t fin_offset() const noexcept {
        return fin_offset_;
    }

    /**
     * Get total bytes consumed (delivered to application).
     */
    uint64_t bytes_consumed() const noexcept {
        return bytes_consumed_;
    }

    /**
     * Get number of segments (gaps + 1).
     */
    size_t segment_count() const noexcept {
        return segment_count_;
    }

    /**
     * Get buffer capacity.
     */
    size_t capacity() const noexcept {
        return BUFFER_SIZE;
    }

    /**
     * Clear buffer to initial state.
     */
    void clear() noexcept {
        segment_count_ = 0;
        bytes_consumed_ = 0;
        largest_received_ = 0;
        fin_offset_ = UINT64_MAX;
        fin_received_ = false;
    }

    /**
     * Statistics for debugging.
     */
    struct Stats {
        size_t segment_count;
        uint64_t bytes_consumed;
        uint64_t largest_received;
        size_t contiguous_available;
        bool fin_received;
    };

    Stats get_stats() const noexcept {
        return Stats{
            segment_count_,
            bytes_consumed_,
            largest_received_,
            contiguous_available(),
            fin_received_
        };
    }

private:
    /**
     * Insert a received segment, coalescing with adjacent/overlapping segments.
     *
     * @return true on success, false if too many segments
     */
    bool insert_segment(uint64_t offset, uint64_t length) noexcept {
        StreamSegment new_seg{offset, length};

        // Fast path: no segments yet
        if (segment_count_ == 0) {
            segments_[0] = new_seg;
            segment_count_ = 1;
            return true;
        }

        // Find the right position and check for overlaps/adjacency
        // Segments are kept sorted by offset

        // Find first segment that could potentially overlap or come after new_seg
        size_t insert_pos = segment_count_;
        for (size_t i = 0; i < segment_count_; i++) {
            // If new segment ends before this one starts (and not adjacent)
            if (new_seg.end() < segments_[i].offset) {
                insert_pos = i;
                break;
            }
            // If new segment overlaps or is adjacent to this one
            if (new_seg.offset <= segments_[i].end()) {
                // Merge with this segment
                uint64_t merged_start = std::min(new_seg.offset, segments_[i].offset);
                uint64_t merged_end = std::max(new_seg.end(), segments_[i].end());
                segments_[i].offset = merged_start;
                segments_[i].length = merged_end - merged_start;

                // Check if we now overlap with subsequent segments
                coalesce_forward(i);
                return true;
            }
        }

        // Check if we can merge with previous segment (adjacent)
        if (insert_pos > 0 && segments_[insert_pos - 1].end() >= new_seg.offset) {
            // Merge with previous
            uint64_t merged_end = std::max(segments_[insert_pos - 1].end(), new_seg.end());
            segments_[insert_pos - 1].length = merged_end - segments_[insert_pos - 1].offset;
            coalesce_forward(insert_pos - 1);
            return true;
        }

        // No overlap - need to insert new segment
        if (segment_count_ >= MAX_SEGMENTS) {
            return false;  // Too many gaps
        }

        // Shift segments to make room
        for (size_t i = segment_count_; i > insert_pos; i--) {
            segments_[i] = segments_[i - 1];
        }
        segments_[insert_pos] = new_seg;
        segment_count_++;

        return true;
    }

    /**
     * Coalesce segment at index with following overlapping/adjacent segments.
     */
    void coalesce_forward(size_t index) noexcept {
        while (index + 1 < segment_count_) {
            StreamSegment& current = segments_[index];
            StreamSegment& next = segments_[index + 1];

            if (current.end() >= next.offset) {
                // Merge with next
                current.length = std::max(current.end(), next.end()) - current.offset;

                // Remove next segment
                for (size_t i = index + 1; i < segment_count_ - 1; i++) {
                    segments_[i] = segments_[i + 1];
                }
                segment_count_--;
            } else {
                break;  // No more overlap
            }
        }
    }

    // Data storage (circular buffer)
    alignas(64) uint8_t buffer_[BUFFER_SIZE];

    // Segment tracking (sorted by offset)
    alignas(64) StreamSegment segments_[MAX_SEGMENTS];
    size_t segment_count_;

    // Stream position tracking
    uint64_t bytes_consumed_;      // Total bytes delivered to application
    uint64_t largest_received_;    // Highest offset + length received
    uint64_t fin_offset_;          // FIN position (UINT64_MAX if not set)
    bool fin_received_;
};

} // namespace quic
} // namespace fasterapi
