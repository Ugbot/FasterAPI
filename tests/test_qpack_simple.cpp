/**
 * Simple QPACK test to isolate encoder/decoder bugs
 */

#include "../src/cpp/http/qpack/qpack_encoder.h"
#include "../src/cpp/http/qpack/qpack_decoder.h"
#include "../src/cpp/http/qpack/qpack_static_table.h"
#include <iostream>
#include <cassert>

using namespace fasterapi::qpack;

void print_hex(const char* label, const uint8_t* data, size_t len) {
    std::cout << label << ": ";
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
    }
    std::cout << std::endl;
}

int main() {
    std::cout << "=== QPACK Encoder/Decoder Bug Investigation ===" << std::endl;

    // Check what index :method GET has
    int idx = QPACKStaticTable::find(":method", "GET");
    std::cout << "\nStatic table index for ':method: GET' = " << idx << std::endl;

    // Check what's at index 17
    const StaticEntry* entry17 = QPACKStaticTable::get(17);
    if (entry17) {
        std::cout << "Static[17] = '" << entry17->name << "': '" << entry17->value << "'" << std::endl;
    }

    // Manually calculate what the encoding should be
    std::cout << "\nExpected encoding for static index 17:" << std::endl;
    std::cout << "  Bit 7: 1 (indexed)" << std::endl;
    std::cout << "  Bit 6: 1 (T=1 for static)" << std::endl;
    std::cout << "  Bits 5-0: " << 17 << " (index)" << std::endl;
    uint8_t expected = 0x80 | 0x40 | 17;
    printf("  Result: 0x%02X = ", expected);
    for (int i = 7; i >= 0; i--) {
        printf("%d", (expected >> i) & 1);
    }
    printf("\n");

    // Now test the encoder
    QPACKEncoder encoder;
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"}
    };

    uint8_t buffer[128];
    size_t encoded_len;

    int result = encoder.encode_field_section(headers, 1, buffer, sizeof(buffer), encoded_len);
    std::cout << "\nEncoder result: " << result << std::endl;
    std::cout << "Encoded length: " << encoded_len << " bytes" << std::endl;
    print_hex("Encoded bytes", buffer, encoded_len);

    // Decode byte 2 (after the prefix bytes)
    if (encoded_len > 2) {
        uint8_t field_byte = buffer[2];
        std::cout << "\nAnalyzing field byte at offset 2: 0x" << std::hex << (int)field_byte << std::dec << std::endl;
        bool indexed = (field_byte & 0x80) != 0;
        bool t_bit = (field_byte & 0x40) != 0;
        int index_val = field_byte & 0x3F;  // Lower 6 bits

        std::cout << "  Indexed: " << indexed << std::endl;
        std::cout << "  T bit (static): " << t_bit << std::endl;
        std::cout << "  Index value: " << index_val << std::endl;

        // Check what that index points to
        const StaticEntry* entry = QPACKStaticTable::get(index_val);
        if (entry) {
            std::cout << "  Points to: '" << entry->name << "': '" << entry->value << "'" << std::endl;
        }
    }

    // Now test the decoder
    QPACKDecoder decoder;
    std::pair<std::string, std::string> decoded[10];
    size_t decoded_count;

    std::cout << "\nDecoding..." << std::endl;
    result = decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
    std::cout << "Decoder result: " << result << std::endl;
    std::cout << "Decoded count: " << decoded_count << std::endl;

    if (result == 0 && decoded_count > 0) {
        std::cout << "Decoded[0]: '" << decoded[0].first << "': '" << decoded[0].second << "'" << std::endl;

        if (decoded[0].first == ":method" && decoded[0].second == "GET") {
            std::cout << "\n✓ Round-trip SUCCESSFUL!" << std::endl;
        } else {
            std::cout << "\n✗ Round-trip FAILED!" << std::endl;
            std::cout << "  Expected: ':method': 'GET'" << std::endl;
            std::cout << "  Got: '" << decoded[0].first << "': '" << decoded[0].second << "'" << std::endl;
        }
    }

    return 0;
}
