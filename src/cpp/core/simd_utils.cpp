#include "simd_utils.h"

namespace fasterapi {
namespace simd {

// ============================================================================
// Scalar fallback implementations
// ============================================================================

namespace scalar {

size_t find_byte(const uint8_t* data, size_t len, uint8_t byte) noexcept {
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == byte) return i;
    }
    return len;
}

size_t find_crlf(const uint8_t* data, size_t len) noexcept {
    if (len < 2) return len;
    for (size_t i = 0; i < len - 1; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    }
    return len;
}

bool ascii_iequals(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) {
        uint8_t ca = a[i];
        uint8_t cb = b[i];
        // Convert to lowercase if uppercase (A-Z -> a-z)
        if (ca >= 'A' && ca <= 'Z') ca |= 0x20;
        if (cb >= 'A' && cb <= 'Z') cb |= 0x20;
        if (ca != cb) return false;
    }
    return true;
}

} // namespace scalar

// ============================================================================
// NEON implementations (ARM64 / Apple Silicon)
// ============================================================================

#if defined(FASTERAPI_SIMD_NEON)

// Minimum buffer size to use SIMD (below this, scalar is faster due to setup overhead)
static constexpr size_t SIMD_MIN_LEN = 32;

size_t find_byte(const uint8_t* data, size_t len, uint8_t byte) noexcept {
    // For small buffers, scalar is faster
    if (len < SIMD_MIN_LEN) {
        return scalar::find_byte(data, len, byte);
    }
    
    size_t i = 0;
    const uint8x16_t needle = vdupq_n_u8(byte);
    
    // SIMD path: process 16 bytes at a time
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8(data + i);
        uint8x16_t cmp = vceqq_u8(chunk, needle);
        
        // Check if any byte matched
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        
        if (lo) {
            // Find first set bit in lower 8 bytes
            int bit = __builtin_ctzll(lo);
            return i + (bit / 8);
        }
        if (hi) {
            int bit = __builtin_ctzll(hi);
            return i + 8 + (bit / 8);
        }
    }
    
    // Scalar tail
    for (; i < len; ++i) {
        if (data[i] == byte) return i;
    }
    return len;
}

size_t find_crlf(const uint8_t* data, size_t len) noexcept {
    if (len < 2) return len;
    
    // For small buffers, scalar is faster
    if (len < SIMD_MIN_LEN) {
        return scalar::find_crlf(data, len);
    }
    
    size_t i = 0;
    const uint8x16_t cr = vdupq_n_u8('\r');
    
    // SIMD path: scan for '\r' then verify '\n'
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8(data + i);
        uint8x16_t cmp = vceqq_u8(chunk, cr);
        
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        
        if (lo | hi) {
            // Found at least one '\r', check each position
            for (size_t j = 0; j < 16 && i + j + 1 < len; ++j) {
                if (data[i + j] == '\r' && data[i + j + 1] == '\n') {
                    return i + j;
                }
            }
        }
    }
    
    // Scalar tail
    for (; i + 1 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    }
    return len;
}

size_t find_delim(const uint8_t* data, size_t len, uint8_t d1, uint8_t d2, uint8_t d3) noexcept {
    // For small buffers, scalar is faster
    if (len < SIMD_MIN_LEN) {
        for (size_t i = 0; i < len; ++i) {
            uint8_t c = data[i];
            if (c == d1 || c == d2 || c == d3) return i;
        }
        return len;
    }
    
    size_t i = 0;
    const uint8x16_t v1 = vdupq_n_u8(d1);
    const uint8x16_t v2 = vdupq_n_u8(d2);
    const uint8x16_t v3 = vdupq_n_u8(d3);
    
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8(data + i);
        
        // OR all comparisons together
        uint8x16_t cmp = vceqq_u8(chunk, v1);
        if (d2) cmp = vorrq_u8(cmp, vceqq_u8(chunk, v2));
        if (d3) cmp = vorrq_u8(cmp, vceqq_u8(chunk, v3));
        
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        
        if (lo) {
            int bit = __builtin_ctzll(lo);
            return i + (bit / 8);
        }
        if (hi) {
            int bit = __builtin_ctzll(hi);
            return i + 8 + (bit / 8);
        }
    }
    
    // Scalar tail
    for (; i < len; ++i) {
        uint8_t c = data[i];
        if (c == d1 || c == d2 || c == d3) return i;
    }
    return len;
}

bool ascii_iequals(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
    size_t i = 0;
    
    if (len >= 16) {
        const uint8x16_t lower_mask = vdupq_n_u8(0x20);
        const uint8x16_t upper_a = vdupq_n_u8('A');
        const uint8x16_t upper_z = vdupq_n_u8('Z');
        
        for (; i + 16 <= len; i += 16) {
            uint8x16_t va = vld1q_u8(a + i);
            uint8x16_t vb = vld1q_u8(b + i);
            
            // Create mask for uppercase letters (A-Z)
            uint8x16_t mask_a = vandq_u8(vcgeq_u8(va, upper_a), vcleq_u8(va, upper_z));
            uint8x16_t mask_b = vandq_u8(vcgeq_u8(vb, upper_a), vcleq_u8(vb, upper_z));
            
            // Convert to lowercase by ORing with 0x20 where uppercase
            va = vorrq_u8(va, vandq_u8(mask_a, lower_mask));
            vb = vorrq_u8(vb, vandq_u8(mask_b, lower_mask));
            
            // Compare
            uint8x16_t cmp = vceqq_u8(va, vb);
            
            // All bytes must match (all 0xFF)
            uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
            uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
            
            if (lo != 0xFFFFFFFFFFFFFFFFULL || hi != 0xFFFFFFFFFFFFFFFFULL) {
                return false;
            }
        }
    }
    
    // Scalar tail
    return scalar::ascii_iequals(a + i, b + i, len - i);
}

size_t find_header_end(const uint8_t* data, size_t len) noexcept {
    if (len < 4) return len;
    
    size_t i = 0;
    
    // SIMD: scan for '\r', then check for \r\n\r\n pattern
    if (len >= 16) {
        const uint8x16_t cr = vdupq_n_u8('\r');
        
        for (; i + 16 <= len; i += 16) {
            uint8x16_t chunk = vld1q_u8(data + i);
            uint8x16_t cmp = vceqq_u8(chunk, cr);
            
            uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
            uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
            
            if (lo | hi) {
                // Check each '\r' position for \r\n\r\n
                for (size_t j = 0; j < 16 && i + j + 3 < len; ++j) {
                    if (data[i + j] == '\r' && 
                        data[i + j + 1] == '\n' &&
                        data[i + j + 2] == '\r' &&
                        data[i + j + 3] == '\n') {
                        return i + j;
                    }
                }
            }
        }
    }
    
    // Scalar tail
    for (; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return i;
        }
    }
    return len;
}

// ============================================================================
// AVX2 implementations (x86-64)
// ============================================================================

#elif defined(FASTERAPI_SIMD_AVX2)

size_t find_byte(const uint8_t* data, size_t len, uint8_t byte) noexcept {
    size_t i = 0;
    
    // SIMD path: process 32 bytes at a time
    if (len >= 32) {
        const __m256i needle = _mm256_set1_epi8(static_cast<char>(byte));
        
        for (; i + 32 <= len; i += 32) {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            __m256i cmp = _mm256_cmpeq_epi8(chunk, needle);
            int mask = _mm256_movemask_epi8(cmp);
            
            if (mask) {
                return i + __builtin_ctz(static_cast<unsigned>(mask));
            }
        }
    }
    
    // Scalar tail
    for (; i < len; ++i) {
        if (data[i] == byte) return i;
    }
    return len;
}

size_t find_crlf(const uint8_t* data, size_t len) noexcept {
    if (len < 2) return len;
    
    size_t i = 0;
    
    // SIMD path: scan for '\r' then verify '\n'
    if (len >= 32) {
        const __m256i cr = _mm256_set1_epi8('\r');
        
        for (; i + 32 <= len; i += 32) {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            __m256i cmp = _mm256_cmpeq_epi8(chunk, cr);
            int mask = _mm256_movemask_epi8(cmp);
            
            while (mask) {
                int pos = __builtin_ctz(static_cast<unsigned>(mask));
                size_t idx = i + pos;
                if (idx + 1 < len && data[idx + 1] == '\n') {
                    return idx;
                }
                mask &= mask - 1; // Clear lowest set bit
            }
        }
    }
    
    // Scalar tail
    for (; i + 1 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    }
    return len;
}

size_t find_delim(const uint8_t* data, size_t len, uint8_t d1, uint8_t d2, uint8_t d3) noexcept {
    size_t i = 0;
    
    if (len >= 32) {
        const __m256i v1 = _mm256_set1_epi8(static_cast<char>(d1));
        const __m256i v2 = _mm256_set1_epi8(static_cast<char>(d2));
        const __m256i v3 = _mm256_set1_epi8(static_cast<char>(d3));
        
        for (; i + 32 <= len; i += 32) {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            
            // OR all comparisons
            __m256i cmp = _mm256_cmpeq_epi8(chunk, v1);
            if (d2) cmp = _mm256_or_si256(cmp, _mm256_cmpeq_epi8(chunk, v2));
            if (d3) cmp = _mm256_or_si256(cmp, _mm256_cmpeq_epi8(chunk, v3));
            
            int mask = _mm256_movemask_epi8(cmp);
            if (mask) {
                return i + __builtin_ctz(static_cast<unsigned>(mask));
            }
        }
    }
    
    // Scalar tail
    for (; i < len; ++i) {
        uint8_t c = data[i];
        if (c == d1 || c == d2 || c == d3) return i;
    }
    return len;
}

bool ascii_iequals(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
    size_t i = 0;
    
    if (len >= 32) {
        const __m256i lower_mask = _mm256_set1_epi8(0x20);
        const __m256i upper_a = _mm256_set1_epi8('A' - 1);  // For > comparison
        const __m256i upper_z = _mm256_set1_epi8('Z' + 1);  // For < comparison
        
        for (; i + 32 <= len; i += 32) {
            __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            
            // Create mask for uppercase: A <= x <= Z
            // Using signed comparison tricks
            __m256i mask_a = _mm256_and_si256(
                _mm256_cmpgt_epi8(va, upper_a),
                _mm256_cmpgt_epi8(upper_z, va)
            );
            __m256i mask_b = _mm256_and_si256(
                _mm256_cmpgt_epi8(vb, upper_a),
                _mm256_cmpgt_epi8(upper_z, vb)
            );
            
            // Convert uppercase to lowercase
            va = _mm256_or_si256(va, _mm256_and_si256(mask_a, lower_mask));
            vb = _mm256_or_si256(vb, _mm256_and_si256(mask_b, lower_mask));
            
            // Compare
            __m256i cmp = _mm256_cmpeq_epi8(va, vb);
            int mask = _mm256_movemask_epi8(cmp);
            
            // All bytes must match (all 1s = 0xFFFFFFFF)
            if (mask != static_cast<int>(0xFFFFFFFF)) {
                return false;
            }
        }
    }
    
    // Scalar tail
    return scalar::ascii_iequals(a + i, b + i, len - i);
}

size_t find_header_end(const uint8_t* data, size_t len) noexcept {
    if (len < 4) return len;
    
    size_t i = 0;
    
    if (len >= 32) {
        const __m256i cr = _mm256_set1_epi8('\r');
        
        for (; i + 32 <= len; i += 32) {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            __m256i cmp = _mm256_cmpeq_epi8(chunk, cr);
            int mask = _mm256_movemask_epi8(cmp);
            
            while (mask) {
                int pos = __builtin_ctz(static_cast<unsigned>(mask));
                size_t idx = i + pos;
                if (idx + 3 < len &&
                    data[idx + 1] == '\n' &&
                    data[idx + 2] == '\r' &&
                    data[idx + 3] == '\n') {
                    return idx;
                }
                mask &= mask - 1;
            }
        }
    }
    
    // Scalar tail
    for (; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return i;
        }
    }
    return len;
}

// ============================================================================
// SSE2 implementations (x86-64 fallback)
// ============================================================================

#elif defined(FASTERAPI_SIMD_SSE2)

size_t find_byte(const uint8_t* data, size_t len, uint8_t byte) noexcept {
    size_t i = 0;
    
    if (len >= 16) {
        const __m128i needle = _mm_set1_epi8(static_cast<char>(byte));
        
        for (; i + 16 <= len; i += 16) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
            __m128i cmp = _mm_cmpeq_epi8(chunk, needle);
            int mask = _mm_movemask_epi8(cmp);
            
            if (mask) {
                return i + __builtin_ctz(static_cast<unsigned>(mask));
            }
        }
    }
    
    for (; i < len; ++i) {
        if (data[i] == byte) return i;
    }
    return len;
}

size_t find_crlf(const uint8_t* data, size_t len) noexcept {
    if (len < 2) return len;
    
    size_t i = 0;
    
    if (len >= 16) {
        const __m128i cr = _mm_set1_epi8('\r');
        
        for (; i + 16 <= len; i += 16) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
            __m128i cmp = _mm_cmpeq_epi8(chunk, cr);
            int mask = _mm_movemask_epi8(cmp);
            
            while (mask) {
                int pos = __builtin_ctz(static_cast<unsigned>(mask));
                size_t idx = i + pos;
                if (idx + 1 < len && data[idx + 1] == '\n') {
                    return idx;
                }
                mask &= mask - 1;
            }
        }
    }
    
    for (; i + 1 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    }
    return len;
}

size_t find_delim(const uint8_t* data, size_t len, uint8_t d1, uint8_t d2, uint8_t d3) noexcept {
    size_t i = 0;
    
    if (len >= 16) {
        const __m128i v1 = _mm_set1_epi8(static_cast<char>(d1));
        const __m128i v2 = _mm_set1_epi8(static_cast<char>(d2));
        const __m128i v3 = _mm_set1_epi8(static_cast<char>(d3));
        
        for (; i + 16 <= len; i += 16) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
            
            __m128i cmp = _mm_cmpeq_epi8(chunk, v1);
            if (d2) cmp = _mm_or_si128(cmp, _mm_cmpeq_epi8(chunk, v2));
            if (d3) cmp = _mm_or_si128(cmp, _mm_cmpeq_epi8(chunk, v3));
            
            int mask = _mm_movemask_epi8(cmp);
            if (mask) {
                return i + __builtin_ctz(static_cast<unsigned>(mask));
            }
        }
    }
    
    for (; i < len; ++i) {
        uint8_t c = data[i];
        if (c == d1 || c == d2 || c == d3) return i;
    }
    return len;
}

bool ascii_iequals(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
    // SSE2 lacks efficient range comparison, use scalar
    return scalar::ascii_iequals(a, b, len);
}

size_t find_header_end(const uint8_t* data, size_t len) noexcept {
    if (len < 4) return len;
    
    size_t i = 0;
    
    if (len >= 16) {
        const __m128i cr = _mm_set1_epi8('\r');
        
        for (; i + 16 <= len; i += 16) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
            __m128i cmp = _mm_cmpeq_epi8(chunk, cr);
            int mask = _mm_movemask_epi8(cmp);
            
            while (mask) {
                int pos = __builtin_ctz(static_cast<unsigned>(mask));
                size_t idx = i + pos;
                if (idx + 3 < len &&
                    data[idx + 1] == '\n' &&
                    data[idx + 2] == '\r' &&
                    data[idx + 3] == '\n') {
                    return idx;
                }
                mask &= mask - 1;
            }
        }
    }
    
    for (; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return i;
        }
    }
    return len;
}

// ============================================================================
// No SIMD - pure scalar
// ============================================================================

#else

size_t find_byte(const uint8_t* data, size_t len, uint8_t byte) noexcept {
    return scalar::find_byte(data, len, byte);
}

size_t find_crlf(const uint8_t* data, size_t len) noexcept {
    return scalar::find_crlf(data, len);
}

size_t find_delim(const uint8_t* data, size_t len, uint8_t d1, uint8_t d2, uint8_t d3) noexcept {
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = data[i];
        if (c == d1 || c == d2 || c == d3) return i;
    }
    return len;
}

bool ascii_iequals(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
    return scalar::ascii_iequals(a, b, len);
}

size_t find_header_end(const uint8_t* data, size_t len) noexcept {
    if (len < 4) return len;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return i;
        }
    }
    return len;
}

#endif

} // namespace simd
} // namespace fasterapi
