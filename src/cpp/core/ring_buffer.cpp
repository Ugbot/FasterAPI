#include "ring_buffer.h"
#include <algorithm>

namespace fasterapi {
namespace core {

// ============================================================================
// MessageBuffer Implementation (Aeron-style)
// ============================================================================

MessageBuffer::MessageBuffer()
    : write_pos_(0), read_pos_(0), claimed_size_(0) {
}

uint8_t* MessageBuffer::claim(size_t size) noexcept {
    if (size > MAX_MESSAGE_SIZE) {
        return nullptr;  // Message too large
    }
    
    // Frame size = 4 bytes (length) + message size + alignment padding
    const size_t frame_size = sizeof(uint32_t) + size;
    const size_t aligned_size = (frame_size + 7) & ~7;  // 8-byte alignment
    
    const uint64_t current_write = write_pos_.load(std::memory_order_relaxed);
    const uint64_t current_read = read_pos_.load(std::memory_order_acquire);
    
    // Check if we have space (accounting for wrap-around)
    const uint64_t available_space = BUFFER_SIZE - (current_write - current_read);
    if (aligned_size > available_space) {
        return nullptr;  // Insufficient space
    }
    
    // Get buffer position (wrap around)
    const size_t buffer_pos = current_write % BUFFER_SIZE;
    
    // Check if we need to wrap
    if (buffer_pos + aligned_size > BUFFER_SIZE) {
        // Not enough contiguous space, would need to handle wrap
        // For simplicity, return nullptr (production would handle wrap)
        return nullptr;
    }
    
    // Write length header
    uint32_t* length_ptr = reinterpret_cast<uint32_t*>(&buffer_[buffer_pos]);
    *length_ptr = static_cast<uint32_t>(size);
    
    // Store claimed size for commit
    claimed_size_ = aligned_size;
    
    // Return pointer to data area (after length)
    return &buffer_[buffer_pos + sizeof(uint32_t)];
}

void MessageBuffer::commit(size_t size) noexcept {
    // Advance write position (makes message visible)
    const uint64_t current_write = write_pos_.load(std::memory_order_relaxed);
    write_pos_.store(current_write + claimed_size_, std::memory_order_release);
    
    claimed_size_ = 0;
}

bool MessageBuffer::read(const uint8_t** out_data, size_t* out_size) noexcept {
    const uint64_t current_read = read_pos_.load(std::memory_order_relaxed);
    const uint64_t current_write = write_pos_.load(std::memory_order_acquire);
    
    // Check if buffer is empty
    if (current_read >= current_write) {
        return false;
    }
    
    // Get buffer position
    const size_t buffer_pos = current_read % BUFFER_SIZE;
    
    // Read length
    const uint32_t* length_ptr = reinterpret_cast<const uint32_t*>(&buffer_[buffer_pos]);
    const uint32_t message_size = *length_ptr;
    
    if (message_size > MAX_MESSAGE_SIZE) {
        // Corrupted data
        return false;
    }
    
    // Return pointer to data
    *out_data = &buffer_[buffer_pos + sizeof(uint32_t)];
    *out_size = message_size;
    
    // Calculate aligned frame size
    const size_t frame_size = sizeof(uint32_t) + message_size;
    const size_t aligned_size = (frame_size + 7) & ~7;
    
    // Advance read position
    read_pos_.store(current_read + aligned_size, std::memory_order_release);
    
    return true;
}

size_t MessageBuffer::available() const noexcept {
    const uint64_t current_write = write_pos_.load(std::memory_order_acquire);
    const uint64_t current_read = read_pos_.load(std::memory_order_acquire);
    return current_write - current_read;
}

} // namespace core
} // namespace fasterapi

