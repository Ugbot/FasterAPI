/**
 * Binary Kwargs Encoder/Decoder Implementation
 *
 * High-performance TLV encoding for Python IPC serialization.
 * Target: ~300ns for 5 simple parameters (26x faster than JSON).
 */

#include "binary_kwargs.h"
#include <cstdlib>
#include <limits>

namespace fasterapi {
namespace python {

// ============================================================================
// Buffer Pool Implementation
// ============================================================================

thread_local std::array<KwargsBufferPool::BufferSlot, KwargsBufferPool::POOL_SIZE>
    KwargsBufferPool::buffers_{};
thread_local size_t KwargsBufferPool::next_slot_ = 0;

uint8_t* KwargsBufferPool::acquire(size_t& capacity) {
    // Linear scan for available slot (fast for small pool)
    size_t start = next_slot_;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        size_t idx = (start + i) % POOL_SIZE;
        bool expected = false;
        if (buffers_[idx].in_use.compare_exchange_strong(
                expected, true, std::memory_order_acquire)) {
            next_slot_ = (idx + 1) % POOL_SIZE;
            capacity = BUFFER_SIZE;
            return buffers_[idx].data;
        }
    }
    // All slots in use - caller should allocate
    capacity = 0;
    return nullptr;
}

void KwargsBufferPool::release(uint8_t* buffer) {
    if (!buffer) return;

    // Find the buffer in our pool
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (buffers_[i].data == buffer) {
            buffers_[i].in_use.store(false, std::memory_order_release);
            return;
        }
    }
    // Not from pool - must have been malloc'd
    std::free(buffer);
}

bool KwargsBufferPool::is_pool_buffer(const uint8_t* buffer) {
    if (!buffer) return false;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (buffers_[i].data == buffer) {
            return true;
        }
    }
    return false;
}

size_t KwargsBufferPool::buffers_in_use() {
    size_t count = 0;
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (buffers_[i].in_use.load(std::memory_order_relaxed)) {
            ++count;
        }
    }
    return count;
}

// ============================================================================
// PooledBuffer Implementation
// ============================================================================

PooledBuffer::PooledBuffer()
    : data_(nullptr), capacity_(0), pooled_(false) {
    data_ = KwargsBufferPool::acquire(capacity_);
    if (data_) {
        pooled_ = true;
    } else {
        // Fallback to malloc
        capacity_ = KwargsBufferPool::BUFFER_SIZE;
        data_ = static_cast<uint8_t*>(std::malloc(capacity_));
        pooled_ = false;
    }
}

PooledBuffer::~PooledBuffer() {
    if (data_) {
        if (pooled_) {
            KwargsBufferPool::release(data_);
        } else {
            std::free(data_);
        }
    }
}

PooledBuffer::PooledBuffer(PooledBuffer&& other) noexcept
    : data_(other.data_), capacity_(other.capacity_), pooled_(other.pooled_) {
    other.data_ = nullptr;
    other.capacity_ = 0;
    other.pooled_ = false;
}

PooledBuffer& PooledBuffer::operator=(PooledBuffer&& other) noexcept {
    if (this != &other) {
        if (data_) {
            if (pooled_) {
                KwargsBufferPool::release(data_);
            } else {
                std::free(data_);
            }
        }
        data_ = other.data_;
        capacity_ = other.capacity_;
        pooled_ = other.pooled_;
        other.data_ = nullptr;
        other.capacity_ = 0;
        other.pooled_ = false;
    }
    return *this;
}

bool PooledBuffer::ensure_capacity(size_t needed) {
    if (needed <= capacity_) return true;

    // Need to grow - always use malloc for larger buffers
    size_t new_capacity = capacity_ * 2;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    uint8_t* new_data = static_cast<uint8_t*>(std::malloc(new_capacity));
    if (!new_data) return false;

    // Copy existing data
    if (data_) {
        std::memcpy(new_data, data_, capacity_);
        if (pooled_) {
            KwargsBufferPool::release(data_);
        } else {
            std::free(data_);
        }
    }

    data_ = new_data;
    capacity_ = new_capacity;
    pooled_ = false;
    return true;
}

// ============================================================================
// BinaryKwargsEncoder Implementation
// ============================================================================

BinaryKwargsEncoder::BinaryKwargsEncoder(PooledBuffer& buffer)
    : buffer_(buffer), pos_(0), param_count_(0), started_(false) {}

void BinaryKwargsEncoder::begin() {
    pos_ = 0;
    param_count_ = 0;
    started_ = true;

    // Write magic byte
    write_u8(BINARY_KWARGS_MAGIC);

    // Reserve space for param count (2 bytes)
    write_u16(0);
}

size_t BinaryKwargsEncoder::finish() {
    if (!started_) return 0;

    // Update param count at position 1
    uint8_t* data = buffer_.data();
    data[1] = static_cast<uint8_t>(param_count_ & 0xFF);
    data[2] = static_cast<uint8_t>((param_count_ >> 8) & 0xFF);

    started_ = false;
    return pos_;
}

bool BinaryKwargsEncoder::ensure_space(size_t needed) {
    return buffer_.ensure_capacity(pos_ + needed);
}

bool BinaryKwargsEncoder::write_name(std::string_view name) {
    if (name.length() > 255) return false;
    if (!ensure_space(1 + name.length())) return false;

    write_u8(static_cast<uint8_t>(name.length()));
    write_bytes(reinterpret_cast<const uint8_t*>(name.data()), name.length());
    return true;
}

void BinaryKwargsEncoder::write_u8(uint8_t value) {
    buffer_.data()[pos_++] = value;
}

void BinaryKwargsEncoder::write_u16(uint16_t value) {
    uint8_t* p = buffer_.data() + pos_;
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    pos_ += 2;
}

void BinaryKwargsEncoder::write_u32(uint32_t value) {
    uint8_t* p = buffer_.data() + pos_;
    p[0] = static_cast<uint8_t>(value & 0xFF);
    p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    pos_ += 4;
}

void BinaryKwargsEncoder::write_u64(uint64_t value) {
    uint8_t* p = buffer_.data() + pos_;
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    pos_ += 8;
}

void BinaryKwargsEncoder::write_i64(int64_t value) {
    write_u64(static_cast<uint64_t>(value));
}

void BinaryKwargsEncoder::write_f64(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    write_u64(bits);
}

void BinaryKwargsEncoder::write_bytes(const uint8_t* data, size_t len) {
    std::memcpy(buffer_.data() + pos_, data, len);
    pos_ += len;
}

bool BinaryKwargsEncoder::add_null(std::string_view name) {
    if (!ensure_space(1 + name.length() + 1)) return false;
    if (!write_name(name)) return false;
    write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_NULL));
    ++param_count_;
    return true;
}

bool BinaryKwargsEncoder::add_bool(std::string_view name, bool value) {
    if (!ensure_space(1 + name.length() + 1)) return false;
    if (!write_name(name)) return false;
    write_u8(static_cast<uint8_t>(value ? KwargsTypeTag::TAG_BOOL_TRUE
                                        : KwargsTypeTag::TAG_BOOL_FALSE));
    ++param_count_;
    return true;
}

bool BinaryKwargsEncoder::add_int(std::string_view name, int64_t value) {
    // Select smallest representation
    if (value >= std::numeric_limits<int8_t>::min() &&
        value <= std::numeric_limits<int8_t>::max()) {
        if (!ensure_space(1 + name.length() + 1 + 1)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_INT8));
        write_u8(static_cast<uint8_t>(static_cast<int8_t>(value)));
    } else if (value >= std::numeric_limits<int16_t>::min() &&
               value <= std::numeric_limits<int16_t>::max()) {
        if (!ensure_space(1 + name.length() + 1 + 2)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_INT16));
        write_u16(static_cast<uint16_t>(static_cast<int16_t>(value)));
    } else if (value >= std::numeric_limits<int32_t>::min() &&
               value <= std::numeric_limits<int32_t>::max()) {
        if (!ensure_space(1 + name.length() + 1 + 4)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_INT32));
        write_u32(static_cast<uint32_t>(static_cast<int32_t>(value)));
    } else {
        if (!ensure_space(1 + name.length() + 1 + 8)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_INT64));
        write_i64(value);
    }
    ++param_count_;
    return true;
}

bool BinaryKwargsEncoder::add_uint(std::string_view name, uint64_t value) {
    // Select smallest representation
    if (value <= std::numeric_limits<uint8_t>::max()) {
        if (!ensure_space(1 + name.length() + 1 + 1)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_UINT8));
        write_u8(static_cast<uint8_t>(value));
    } else if (value <= std::numeric_limits<uint16_t>::max()) {
        if (!ensure_space(1 + name.length() + 1 + 2)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_UINT16));
        write_u16(static_cast<uint16_t>(value));
    } else if (value <= std::numeric_limits<uint32_t>::max()) {
        if (!ensure_space(1 + name.length() + 1 + 4)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_UINT32));
        write_u32(static_cast<uint32_t>(value));
    } else {
        if (!ensure_space(1 + name.length() + 1 + 8)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_UINT64));
        write_u64(value);
    }
    ++param_count_;
    return true;
}

bool BinaryKwargsEncoder::add_float(std::string_view name, double value) {
    if (!ensure_space(1 + name.length() + 1 + 8)) return false;
    if (!write_name(name)) return false;
    write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_FLOAT64));
    write_f64(value);
    ++param_count_;
    return true;
}

bool BinaryKwargsEncoder::add_string(std::string_view name, std::string_view value) {
    // Select smallest length encoding
    if (value.length() <= 255) {
        if (!ensure_space(1 + name.length() + 1 + 1 + value.length())) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_STR_TINY));
        write_u8(static_cast<uint8_t>(value.length()));
    } else if (value.length() <= 65535) {
        if (!ensure_space(1 + name.length() + 1 + 2 + value.length())) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_STR_SHORT));
        write_u16(static_cast<uint16_t>(value.length()));
    } else {
        if (!ensure_space(1 + name.length() + 1 + 4 + value.length())) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_STR_MEDIUM));
        write_u32(static_cast<uint32_t>(value.length()));
    }
    write_bytes(reinterpret_cast<const uint8_t*>(value.data()), value.length());
    ++param_count_;
    return true;
}

bool BinaryKwargsEncoder::add_bytes(std::string_view name, const uint8_t* data, size_t len) {
    // Select smallest length encoding
    if (len <= 255) {
        if (!ensure_space(1 + name.length() + 1 + 1 + len)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_BYTES_TINY));
        write_u8(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
        if (!ensure_space(1 + name.length() + 1 + 2 + len)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_BYTES_SHORT));
        write_u16(static_cast<uint16_t>(len));
    } else {
        if (!ensure_space(1 + name.length() + 1 + 4 + len)) return false;
        if (!write_name(name)) return false;
        write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_BYTES_MEDIUM));
        write_u32(static_cast<uint32_t>(len));
    }
    write_bytes(data, len);
    ++param_count_;
    return true;
}

bool BinaryKwargsEncoder::add_json_fallback(std::string_view name, std::string_view json) {
    if (!ensure_space(1 + name.length() + 1 + 4 + json.length())) return false;
    if (!write_name(name)) return false;
    write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_JSON));
    write_u32(static_cast<uint32_t>(json.length()));
    write_bytes(reinterpret_cast<const uint8_t*>(json.data()), json.length());
    ++param_count_;
    return true;
}

bool BinaryKwargsEncoder::add_msgpack_fallback(std::string_view name,
                                                const uint8_t* data, size_t len) {
    if (!ensure_space(1 + name.length() + 1 + 4 + len)) return false;
    if (!write_name(name)) return false;
    write_u8(static_cast<uint8_t>(KwargsTypeTag::TAG_MSGPACK));
    write_u32(static_cast<uint32_t>(len));
    write_bytes(data, len);
    ++param_count_;
    return true;
}

// ============================================================================
// BinaryKwargsDecoder Implementation
// ============================================================================

bool BinaryKwargsDecoder::init(const uint8_t* data, size_t len) {
    if (len < 3) return false;  // Magic + 2-byte param count
    if (data[0] != BINARY_KWARGS_MAGIC) return false;

    data_ = data;
    len_ = len;
    pos_ = 3;  // Skip magic + param count

    param_count_ = static_cast<uint16_t>(data[1]) |
                   (static_cast<uint16_t>(data[2]) << 8);
    params_read_ = 0;

    return true;
}

void BinaryKwargsDecoder::reset() {
    pos_ = 3;
    params_read_ = 0;
}

bool BinaryKwargsDecoder::next(Parameter& param) {
    if (params_read_ >= param_count_ || pos_ >= len_) {
        return false;
    }

    // Read name
    if (pos_ >= len_) return false;
    uint8_t name_len = data_[pos_++];
    if (pos_ + name_len > len_) return false;
    param.name = std::string_view(reinterpret_cast<const char*>(data_ + pos_), name_len);
    pos_ += name_len;

    // Read tag
    if (pos_ >= len_) return false;
    param.tag = static_cast<KwargsTypeTag>(data_[pos_++]);

    // Read value based on tag
    switch (param.tag) {
        case KwargsTypeTag::TAG_NULL:
            break;

        case KwargsTypeTag::TAG_BOOL_FALSE:
            param.bool_val = false;
            break;

        case KwargsTypeTag::TAG_BOOL_TRUE:
            param.bool_val = true;
            break;

        case KwargsTypeTag::TAG_INT8:
            if (pos_ + 1 > len_) return false;
            param.int_val = static_cast<int8_t>(data_[pos_++]);
            break;

        case KwargsTypeTag::TAG_INT16:
            if (pos_ + 2 > len_) return false;
            param.int_val = static_cast<int16_t>(
                static_cast<uint16_t>(data_[pos_]) |
                (static_cast<uint16_t>(data_[pos_ + 1]) << 8));
            pos_ += 2;
            break;

        case KwargsTypeTag::TAG_INT32:
            if (pos_ + 4 > len_) return false;
            param.int_val = static_cast<int32_t>(
                static_cast<uint32_t>(data_[pos_]) |
                (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                (static_cast<uint32_t>(data_[pos_ + 3]) << 24));
            pos_ += 4;
            break;

        case KwargsTypeTag::TAG_INT64:
            if (pos_ + 8 > len_) return false;
            {
                uint64_t val = 0;
                for (int i = 0; i < 8; ++i) {
                    val |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
                }
                param.int_val = static_cast<int64_t>(val);
            }
            pos_ += 8;
            break;

        case KwargsTypeTag::TAG_UINT8:
            if (pos_ + 1 > len_) return false;
            param.uint_val = data_[pos_++];
            break;

        case KwargsTypeTag::TAG_UINT16:
            if (pos_ + 2 > len_) return false;
            param.uint_val = static_cast<uint16_t>(data_[pos_]) |
                            (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
            pos_ += 2;
            break;

        case KwargsTypeTag::TAG_UINT32:
            if (pos_ + 4 > len_) return false;
            param.uint_val = static_cast<uint32_t>(data_[pos_]) |
                            (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                            (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                            (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
            pos_ += 4;
            break;

        case KwargsTypeTag::TAG_UINT64:
            if (pos_ + 8 > len_) return false;
            {
                uint64_t val = 0;
                for (int i = 0; i < 8; ++i) {
                    val |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
                }
                param.uint_val = val;
            }
            pos_ += 8;
            break;

        case KwargsTypeTag::TAG_FLOAT32:
            if (pos_ + 4 > len_) return false;
            {
                uint32_t bits = static_cast<uint32_t>(data_[pos_]) |
                               (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                               (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                               (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
                float f;
                std::memcpy(&f, &bits, sizeof(f));
                param.float_val = static_cast<double>(f);
            }
            pos_ += 4;
            break;

        case KwargsTypeTag::TAG_FLOAT64:
            if (pos_ + 8 > len_) return false;
            {
                uint64_t bits = 0;
                for (int i = 0; i < 8; ++i) {
                    bits |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
                }
                std::memcpy(&param.float_val, &bits, sizeof(param.float_val));
            }
            pos_ += 8;
            break;

        case KwargsTypeTag::TAG_STR_TINY: {
            if (pos_ + 1 > len_) return false;
            size_t str_len = data_[pos_++];
            if (pos_ + str_len > len_) return false;
            param.str_val = std::string_view(
                reinterpret_cast<const char*>(data_ + pos_), str_len);
            pos_ += str_len;
            break;
        }

        case KwargsTypeTag::TAG_STR_SHORT: {
            if (pos_ + 2 > len_) return false;
            size_t str_len = static_cast<uint16_t>(data_[pos_]) |
                            (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
            pos_ += 2;
            if (pos_ + str_len > len_) return false;
            param.str_val = std::string_view(
                reinterpret_cast<const char*>(data_ + pos_), str_len);
            pos_ += str_len;
            break;
        }

        case KwargsTypeTag::TAG_STR_MEDIUM: {
            if (pos_ + 4 > len_) return false;
            size_t str_len = static_cast<uint32_t>(data_[pos_]) |
                            (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                            (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                            (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
            pos_ += 4;
            if (pos_ + str_len > len_) return false;
            param.str_val = std::string_view(
                reinterpret_cast<const char*>(data_ + pos_), str_len);
            pos_ += str_len;
            break;
        }

        case KwargsTypeTag::TAG_BYTES_TINY: {
            if (pos_ + 1 > len_) return false;
            size_t bytes_len = data_[pos_++];
            if (pos_ + bytes_len > len_) return false;
            param.bytes_ptr = data_ + pos_;
            param.bytes_len = bytes_len;
            pos_ += bytes_len;
            break;
        }

        case KwargsTypeTag::TAG_BYTES_SHORT: {
            if (pos_ + 2 > len_) return false;
            size_t bytes_len = static_cast<uint16_t>(data_[pos_]) |
                              (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
            pos_ += 2;
            if (pos_ + bytes_len > len_) return false;
            param.bytes_ptr = data_ + pos_;
            param.bytes_len = bytes_len;
            pos_ += bytes_len;
            break;
        }

        case KwargsTypeTag::TAG_BYTES_MEDIUM: {
            if (pos_ + 4 > len_) return false;
            size_t bytes_len = static_cast<uint32_t>(data_[pos_]) |
                              (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                              (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                              (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
            pos_ += 4;
            if (pos_ + bytes_len > len_) return false;
            param.bytes_ptr = data_ + pos_;
            param.bytes_len = bytes_len;
            pos_ += bytes_len;
            break;
        }

        case KwargsTypeTag::TAG_MSGPACK:
        case KwargsTypeTag::TAG_JSON: {
            if (pos_ + 4 > len_) return false;
            size_t data_len = static_cast<uint32_t>(data_[pos_]) |
                             (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                             (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                             (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
            pos_ += 4;
            if (pos_ + data_len > len_) return false;
            param.bytes_ptr = data_ + pos_;
            param.bytes_len = data_len;
            param.str_val = std::string_view(
                reinterpret_cast<const char*>(data_ + pos_), data_len);
            pos_ += data_len;
            break;
        }

        default:
            return false;  // Unknown tag
    }

    ++params_read_;
    return true;
}

// ============================================================================
// Utility Functions
// ============================================================================

size_t encode_simple_kwargs(
    PooledBuffer& buffer,
    const std::pair<std::string_view, std::string_view>* params,
    size_t param_count) {

    BinaryKwargsEncoder encoder(buffer);
    encoder.begin();

    for (size_t i = 0; i < param_count; ++i) {
        if (!encoder.add_string(params[i].first, params[i].second)) {
            return 0;
        }
    }

    return encoder.finish();
}

}  // namespace python
}  // namespace fasterapi
