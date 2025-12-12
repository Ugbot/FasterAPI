#pragma once

#include <cstddef>
#include <cstdint>

// Detect SIMD capabilities at compile time
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    #include <arm_neon.h>
    #define FASTERAPI_SIMD_NEON 1
    #define FASTERAPI_SIMD_WIDTH 16
#elif defined(__AVX2__)
    #include <immintrin.h>
    #define FASTERAPI_SIMD_AVX2 1
    #define FASTERAPI_SIMD_WIDTH 32
#elif defined(__SSE2__)
    #include <emmintrin.h>
    #define FASTERAPI_SIMD_SSE2 1
    #define FASTERAPI_SIMD_WIDTH 16
#else
    #define FASTERAPI_SIMD_NONE 1
    #define FASTERAPI_SIMD_WIDTH 1
#endif

namespace fasterapi {
namespace simd {

/**
 * Find first occurrence of a byte in a buffer.
 * Returns offset of the byte, or len if not found.
 * 
 * Uses SIMD to scan FASTERAPI_SIMD_WIDTH bytes at a time.
 */
size_t find_byte(const uint8_t* data, size_t len, uint8_t byte) noexcept;

/**
 * Find first CRLF sequence (\r\n) in a buffer.
 * Returns offset of the '\r', or len if not found.
 * 
 * Uses SIMD to scan for '\r' then verifies next byte is '\n'.
 */
size_t find_crlf(const uint8_t* data, size_t len) noexcept;

/**
 * Find first occurrence of any byte from a set of delimiters.
 * Returns offset of the delimiter, or len if not found.
 * 
 * Optimized for common HTTP delimiters: space, colon, CRLF.
 */
size_t find_delim(const uint8_t* data, size_t len, uint8_t d1, uint8_t d2 = 0, uint8_t d3 = 0) noexcept;

/**
 * Case-insensitive comparison of two ASCII strings.
 * Only works correctly for ASCII (a-z, A-Z).
 * Returns true if equal (ignoring case).
 */
bool ascii_iequals(const uint8_t* a, const uint8_t* b, size_t len) noexcept;

/**
 * Find double CRLF sequence (\r\n\r\n) marking end of HTTP headers.
 * Returns offset of the first '\r', or len if not found.
 */
size_t find_header_end(const uint8_t* data, size_t len) noexcept;

// Scalar fallback implementations (always available for testing)
namespace scalar {
    size_t find_byte(const uint8_t* data, size_t len, uint8_t byte) noexcept;
    size_t find_crlf(const uint8_t* data, size_t len) noexcept;
    bool ascii_iequals(const uint8_t* a, const uint8_t* b, size_t len) noexcept;
}

} // namespace simd
} // namespace fasterapi
