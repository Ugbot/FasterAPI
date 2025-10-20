#pragma once

#include <cstdint>
#include <cstddef>

namespace fasterapi {
namespace http {

/**
 * Huffman encoder/decoder for HPACK.
 * 
 * Based on nghttp2's Huffman implementation.
 * Adapted for zero-allocation, stack-only operation.
 * 
 * Spec: RFC 7541 Appendix B (HPACK Huffman Code)
 * 
 * Performance targets:
 * - Encode: <50ns per byte
 * - Decode: <80ns per byte
 * - Zero allocations
 * 
 * Compression ratio: ~30-40% for typical headers
 */

/**
 * Huffman encoder.
 */
class HuffmanEncoder {
public:
    /**
     * Encode data using Huffman coding.
     * 
     * @param input Input data
     * @param input_len Input length
     * @param output Output buffer
     * @param output_capacity Output buffer capacity
     * @param out_encoded_len Encoded length
     * @return 0 on success, 1 if output buffer too small
     */
    static int encode(
        const uint8_t* input,
        size_t input_len,
        uint8_t* output,
        size_t output_capacity,
        size_t& out_encoded_len
    ) noexcept;
    
    /**
     * Get encoded size (without actually encoding).
     * 
     * Useful for buffer allocation.
     * 
     * @param input Input data
     * @param input_len Input length
     * @return Encoded size in bytes
     */
    static size_t encoded_size(
        const uint8_t* input,
        size_t input_len
    ) noexcept;
};

/**
 * Huffman decoder.
 */
class HuffmanDecoder {
public:
    /**
     * Decode Huffman-encoded data.
     * 
     * @param input Encoded data
     * @param input_len Encoded length
     * @param output Output buffer
     * @param output_capacity Output buffer capacity
     * @param out_decoded_len Decoded length
     * @return 0 on success, 1 on error
     */
    static int decode(
        const uint8_t* input,
        size_t input_len,
        uint8_t* output,
        size_t output_capacity,
        size_t& out_decoded_len
    ) noexcept;
    
private:
    /**
     * Huffman decode state machine.
     */
    struct DecodeState {
        uint8_t state;
        uint8_t flags;
    };
    
    // Decode table (from nghttp2)
    static const DecodeState decode_table_[256][16];
};

} // namespace http
} // namespace fasterapi

