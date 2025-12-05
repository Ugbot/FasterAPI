#include "../src/cpp/http/qpack/qpack_decoder.h"
#include "../src/cpp/http/qpack/qpack_encoder.h"
#include <iostream>
#include <iomanip>

using namespace fasterapi::qpack;

void print_hex(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
    }
    std::cout << std::dec << std::endl;
}

int main() {
    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);

    // Test static table lookup
    std::cout << "Checking static table for :method POST..." << std::endl;
    int idx = QPACKStaticTable::find(":method", "POST");
    std::cout << "Found at index: " << idx << std::endl;

    const StaticEntry* entry = QPACKStaticTable::get(20);
    if (entry) {
        std::cout << "Static[20]: " << entry->name << " = " << entry->value << std::endl;
    }

    // Test: :method POST
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "POST"}
    };

    uint8_t encoded[1024];
    size_t encoded_len;

    std::cout << "\nEncoding :method POST..." << std::endl;
    int result = encoder.encode_field_section(headers, 1, encoded, sizeof(encoded), encoded_len);
    std::cout << "Encode result: " << result << ", length: " << encoded_len << std::endl;
    std::cout << "Encoded bytes: ";
    print_hex(encoded, encoded_len);

    // Try to decode
    std::pair<std::string, std::string> decoded[10];
    size_t count;

    std::cout << "\nDecoding..." << std::endl;
    result = decoder.decode_field_section(encoded, encoded_len, decoded, count);
    std::cout << "Decode result: " << result << ", count: " << count << std::endl;

    if (result == 0 && count > 0) {
        std::cout << "Decoded header: " << decoded[0].first << " = " << decoded[0].second << std::endl;
    }

    // Test invalid index
    std::cout << "\n\nTesting invalid static index 131..." << std::endl;
    uint8_t bad_encoded[] = {0x00, 0x00, 0xFF, 0x20};
    std::cout << "Bad encoded bytes: ";
    print_hex(bad_encoded, sizeof(bad_encoded));

    result = decoder.decode_field_section(bad_encoded, sizeof(bad_encoded), decoded, count);
    std::cout << "Decode result (should be -1): " << result << std::endl;

    return 0;
}
