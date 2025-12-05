// Test suite for QUIC packet parsing and serialization
// Tests RFC 9000 compliance with randomized inputs

#include "quic_packet.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace fasterapi::quic;

// Test utilities
std::mt19937 rng(42);  // Deterministic for reproducibility

uint8_t random_byte() {
    return static_cast<uint8_t>(rng() & 0xFF);
}

uint32_t random_u32() {
    return static_cast<uint32_t>(rng());
}

uint64_t random_u64() {
    return (static_cast<uint64_t>(rng()) << 32) | rng();
}

void fill_random_bytes(uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] = random_byte();
    }
}

// Test 1: Long header Initial packet parsing
void test_long_header_initial() {
    printf("Test 1: Long header Initial packet parsing...\n");

    // Create a valid Initial packet
    uint8_t packet[] = {
        0xC0,  // Long header, Initial packet (11|00|0000)
        0x00, 0x00, 0x00, 0x01,  // Version 1
        0x08,  // DCID length
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // DCID
        0x04,  // SCID length
        0x11, 0x22, 0x33, 0x44,  // SCID
        0x00,  // Token length (varint 0)
        0x05,  // Packet length (varint 5)
        // Packet number and payload would follow (encrypted in real scenario)
    };

    LongHeader header;
    size_t consumed = 0;
    int result = header.parse(packet, sizeof(packet), consumed);

    assert(result == 0);
    assert(header.type == PacketType::INITIAL);
    assert(header.version == 1);
    assert(header.dest_conn_id.length == 8);
    assert(header.source_conn_id.length == 4);
    assert(header.token_length == 0);
    assert(header.packet_length == 5);

    // Verify connection IDs
    assert(header.dest_conn_id.data[0] == 0x01);
    assert(header.dest_conn_id.data[7] == 0x08);
    assert(header.source_conn_id.data[0] == 0x11);
    assert(header.source_conn_id.data[3] == 0x44);

    printf("  ✓ Initial packet parsed correctly\n");
}

// Test 2: Long header with token
void test_long_header_with_token() {
    printf("Test 2: Long header with token...\n");

    uint8_t token_data[16];
    fill_random_bytes(token_data, sizeof(token_data));

    std::vector<uint8_t> packet;
    packet.push_back(0xC0);  // Initial packet
    packet.push_back(0x00); packet.push_back(0x00);
    packet.push_back(0x00); packet.push_back(0x01);  // Version 1

    // DCID
    packet.push_back(0x08);
    for (int i = 0; i < 8; i++) packet.push_back(random_byte());

    // SCID
    packet.push_back(0x00);  // Zero-length SCID

    // Token
    packet.push_back(0x10);  // Token length (varint 16)
    for (int i = 0; i < 16; i++) packet.push_back(token_data[i]);

    // Packet length
    packet.push_back(0x0A);  // Packet length

    LongHeader header;
    size_t consumed = 0;
    int result = header.parse(packet.data(), packet.size(), consumed);

    assert(result == 0);
    assert(header.type == PacketType::INITIAL);
    assert(header.token_length == 16);
    assert(std::memcmp(header.token, token_data, 16) == 0);

    printf("  ✓ Token parsed correctly\n");
}

// Test 3: Short header parsing
void test_short_header() {
    printf("Test 3: Short header parsing...\n");

    uint8_t packet[] = {
        0x43,  // Short header: 0|1|0|00|0|11 (4-byte packet number)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // DCID (8 bytes)
        0x12, 0x34, 0x56, 0x78,  // Packet number (4 bytes)
    };

    ShortHeader header;
    size_t consumed = 0;
    int result = header.parse(packet, sizeof(packet), 8, consumed);

    assert(result == 0);
    assert(header.spin_bit == false);
    assert(header.key_phase == false);
    assert(header.packet_number_length == 4);
    assert(header.packet_number == 0x12345678);
    assert(header.dest_conn_id.length == 8);

    printf("  ✓ Short header parsed correctly\n");
}

// Test 4: Long header serialization round-trip
void test_long_header_serialization() {
    printf("Test 4: Long header serialization round-trip...\n");

    // Create a header
    LongHeader orig;
    orig.type = PacketType::HANDSHAKE;
    orig.version = 1;
    orig.dest_conn_id = ConnectionID((const uint8_t*)"\x01\x02\x03\x04", 4);
    orig.source_conn_id = ConnectionID((const uint8_t*)"\x11\x12\x13\x14\x15", 5);
    orig.token_length = 0;
    orig.token = nullptr;
    orig.packet_length = 100;

    // Serialize
    uint8_t buffer[256];
    size_t written = orig.serialize(buffer);

    // Parse it back
    LongHeader parsed;
    size_t consumed = 0;
    int result = parsed.parse(buffer, written, consumed);

    assert(result == 0);
    assert(parsed.type == orig.type);
    assert(parsed.version == orig.version);
    assert(parsed.dest_conn_id == orig.dest_conn_id);
    assert(parsed.source_conn_id == orig.source_conn_id);
    assert(parsed.packet_length == orig.packet_length);

    printf("  ✓ Serialization round-trip successful\n");
}

// Test 5: Short header serialization round-trip
void test_short_header_serialization() {
    printf("Test 5: Short header serialization round-trip...\n");

    // Create multiple headers with different packet number lengths
    for (int pn_len = 1; pn_len <= 4; pn_len++) {
        ShortHeader orig;
        orig.spin_bit = (pn_len % 2 == 0);
        orig.key_phase = (pn_len > 2);
        orig.dest_conn_id = ConnectionID((const uint8_t*)"\xAA\xBB\xCC\xDD\xEE\xFF", 6);
        orig.packet_number_length = pn_len;

        // Use packet number that fits in pn_len bytes
        uint64_t max_pn = (1ULL << (pn_len * 8)) - 1;
        orig.packet_number = random_u64() & max_pn;

        // Serialize
        uint8_t buffer[128];
        size_t written = orig.serialize(buffer);

        // Parse it back
        ShortHeader parsed;
        size_t consumed = 0;
        int result = parsed.parse(buffer, written, 6, consumed);

        assert(result == 0);
        assert(parsed.spin_bit == orig.spin_bit);
        assert(parsed.key_phase == orig.key_phase);
        assert(parsed.packet_number_length == orig.packet_number_length);
        assert(parsed.packet_number == orig.packet_number);
        assert(parsed.dest_conn_id == orig.dest_conn_id);

        printf("  ✓ Round-trip with %d-byte packet number\n", pn_len);
    }
}

// Test 6: Packet number encoding/decoding
void test_packet_number_encoding() {
    printf("Test 6: Packet number encoding/decoding...\n");

    // Test encode_packet_number_length
    assert(encode_packet_number_length(0x00) == 1);
    assert(encode_packet_number_length(0xFF) == 1);
    assert(encode_packet_number_length(0x100) == 2);
    assert(encode_packet_number_length(0xFFFF) == 2);
    assert(encode_packet_number_length(0x10000) == 3);
    assert(encode_packet_number_length(0xFFFFFF) == 3);
    assert(encode_packet_number_length(0x1000000) == 4);

    printf("  ✓ Packet number length encoding correct\n");

    // Test packet number reconstruction (RFC 9000 Appendix A.3 examples)
    // Example: largest_acked = 0xaa82f30e
    uint64_t largest_acked = 0xaa82f30e;

    // Truncated value 0x9b32 (2 bytes = 16 bits) should decode to 0xaa8309b32
    uint64_t decoded = decode_packet_number(0x9b32, largest_acked, 16);
    assert(decoded == 0xaa829b32);

    printf("  ✓ Packet number reconstruction correct\n");
}

// Test 7: Complete packet parsing with payload
void test_complete_packet_parsing() {
    printf("Test 7: Complete packet parsing...\n");

    // Create a long header packet with payload
    uint8_t payload_data[32];
    fill_random_bytes(payload_data, sizeof(payload_data));

    std::vector<uint8_t> packet_buffer;
    packet_buffer.push_back(0xC0);  // Initial packet
    packet_buffer.push_back(0x00); packet_buffer.push_back(0x00);
    packet_buffer.push_back(0x00); packet_buffer.push_back(0x01);

    // DCID
    packet_buffer.push_back(0x08);
    for (int i = 0; i < 8; i++) packet_buffer.push_back(i);

    // SCID
    packet_buffer.push_back(0x00);

    // Token
    packet_buffer.push_back(0x00);

    // Packet length (payload size)
    packet_buffer.push_back(sizeof(payload_data));

    // Add payload
    for (size_t i = 0; i < sizeof(payload_data); i++) {
        packet_buffer.push_back(payload_data[i]);
    }

    // Parse complete packet
    Packet packet;
    size_t consumed = 0;
    int result = parse_packet(packet_buffer.data(), packet_buffer.size(), 8, packet, consumed);

    assert(result == 0);
    assert(packet.is_long_header);
    assert(packet.payload_length == sizeof(payload_data));
    assert(std::memcmp(packet.payload, payload_data, sizeof(payload_data)) == 0);
    assert(consumed == packet_buffer.size());

    printf("  ✓ Complete packet with payload parsed\n");
}

// Test 8: Validation helpers
void test_validation_helpers() {
    printf("Test 8: Validation helpers...\n");

    // Test version validation
    assert(validate_version(0x00000001) == true);   // QUIC v1
    assert(validate_version(0x00000000) == true);   // Version negotiation
    assert(validate_version(0x0a0a0a0a) == true);   // Reserved
    assert(validate_version(0x12345678) == false);  // Unknown

    // Test fixed bit validation
    assert(validate_fixed_bit(0xC0) == true);   // Long header with fixed bit
    assert(validate_fixed_bit(0x40) == true);   // Short header with fixed bit
    assert(validate_fixed_bit(0x80) == false);  // Long header without fixed bit
    assert(validate_fixed_bit(0x00) == false);  // No fixed bit

    // Test header type detection
    assert(is_long_header(0xC0) == true);
    assert(is_long_header(0x80) == true);
    assert(is_long_header(0x40) == false);
    assert(is_long_header(0x00) == false);

    printf("  ✓ Validation helpers working\n");
}

// Test 9: Connection ID helpers
void test_connection_id_helpers() {
    printf("Test 9: Connection ID helpers...\n");

    ConnectionID cid1((const uint8_t*)"\x01\x02\x03", 3);
    ConnectionID cid2((const uint8_t*)"\x01\x02\x03", 3);
    ConnectionID cid3((const uint8_t*)"\x01\x02\x04", 3);
    ConnectionID cid4((const uint8_t*)"\x01\x02", 2);

    assert(cid1 == cid2);
    assert(cid1 != cid3);
    assert(cid1 != cid4);

    assert(compare_connection_id(cid1, cid2) == 0);
    assert(compare_connection_id(cid1, cid3) < 0);
    assert(compare_connection_id(cid3, cid1) > 0);
    assert(compare_connection_id(cid1, cid4) > 0);

    printf("  ✓ Connection ID comparison working\n");
}

// Test 10: Packet type helpers
void test_packet_type_helpers() {
    printf("Test 10: Packet type helpers...\n");

    assert(strcmp(packet_type_to_string(PacketType::INITIAL), "Initial") == 0);
    assert(strcmp(packet_type_to_string(PacketType::ZERO_RTT), "0-RTT") == 0);
    assert(strcmp(packet_type_to_string(PacketType::HANDSHAKE), "Handshake") == 0);
    assert(strcmp(packet_type_to_string(PacketType::RETRY), "Retry") == 0);
    assert(strcmp(packet_type_to_string(PacketType::ONE_RTT), "1-RTT") == 0);

    assert(packet_type_has_token(PacketType::INITIAL) == true);
    assert(packet_type_has_token(PacketType::HANDSHAKE) == false);

    assert(packet_type_has_packet_number(PacketType::INITIAL) == true);
    assert(packet_type_has_packet_number(PacketType::RETRY) == false);

    printf("  ✓ Packet type helpers working\n");
}

// Test 11: Buffer size estimation
void test_buffer_size_estimation() {
    printf("Test 11: Buffer size estimation...\n");

    size_t size1 = estimate_long_header_size(PacketType::INITIAL, 8, 8, 100);
    assert(size1 > 0);

    size_t size2 = estimate_long_header_size(PacketType::HANDSHAKE, 8, 8, 0);
    assert(size2 > 0);
    assert(size2 < size1);  // No token, so smaller

    size_t size3 = estimate_short_header_size(8);
    assert(size3 > 0);
    assert(size3 < size2);  // Short header is smaller

    printf("  ✓ Buffer size estimation working\n");
}

// Test 12: Error handling - insufficient data
void test_error_handling_insufficient_data() {
    printf("Test 12: Error handling - insufficient data...\n");

    uint8_t packet[] = {
        0xC0,  // Long header
        0x00, 0x00,  // Incomplete version
    };

    LongHeader header;
    size_t consumed = 0;
    int result = header.parse(packet, sizeof(packet), consumed);

    assert(result == -1);  // Need more data

    printf("  ✓ Insufficient data detected\n");
}

// Test 13: Error handling - invalid fixed bit
void test_error_handling_invalid_fixed_bit() {
    printf("Test 13: Error handling - invalid fixed bit...\n");

    uint8_t packet[] = {
        0x80,  // Long header WITHOUT fixed bit (invalid)
        0x00, 0x00, 0x00, 0x01,
        // ...
    };

    Packet pkt;
    size_t consumed = 0;
    int result = parse_packet(packet, sizeof(packet), 8, pkt, consumed);

    assert(result == 1);  // Invalid packet

    printf("  ✓ Invalid fixed bit detected\n");
}

// Test 14: Diagnostic functions
void test_diagnostic_functions() {
    printf("Test 14: Diagnostic functions...\n");

    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    uint32_t checksum = calculate_packet_checksum(data, sizeof(data));
    assert(checksum != 0);

    // Test packet dump
    Packet packet;
    packet.is_long_header = true;
    packet.long_hdr.type = PacketType::INITIAL;
    packet.long_hdr.version = 1;
    packet.long_hdr.dest_conn_id = ConnectionID((const uint8_t*)"\x01\x02", 2);
    packet.long_hdr.source_conn_id = ConnectionID((const uint8_t*)"\x11\x12", 2);
    packet.long_hdr.packet_length = 100;
    packet.long_hdr.packet_number = 42;
    packet.payload_length = 100;

    char buffer[512];
    int written = dump_packet_header(packet, buffer, sizeof(buffer));
    assert(written > 0);
    assert(strstr(buffer, "Initial") != nullptr);
    assert(strstr(buffer, "0x00000001") != nullptr);

    printf("  ✓ Diagnostic functions working\n");
}

// Test 15: Randomized stress test
void test_randomized_stress() {
    printf("Test 15: Randomized stress test...\n");

    for (int i = 0; i < 100; i++) {
        // Random packet type
        PacketType type = static_cast<PacketType>(rng() % 4);

        // Random connection ID lengths
        uint8_t dcid_len = rng() % 21;
        uint8_t scid_len = rng() % 21;

        // Create random connection IDs
        uint8_t dcid_data[20], scid_data[20];
        fill_random_bytes(dcid_data, dcid_len);
        fill_random_bytes(scid_data, scid_len);

        LongHeader header;
        header.type = type;
        header.version = 1;
        header.dest_conn_id = ConnectionID(dcid_data, dcid_len);
        header.source_conn_id = ConnectionID(scid_data, scid_len);
        header.token_length = 0;
        header.packet_length = rng() % 1200;

        // Serialize and parse back
        uint8_t buffer[2048];
        size_t written = header.serialize(buffer);

        LongHeader parsed;
        size_t consumed = 0;
        int result = parsed.parse(buffer, written, consumed);

        assert(result == 0);
        assert(parsed.type == header.type);
        assert(parsed.version == header.version);
        assert(parsed.dest_conn_id == header.dest_conn_id);
        assert(parsed.source_conn_id == header.source_conn_id);
    }

    printf("  ✓ 100 randomized round-trips successful\n");
}

int main() {
    printf("=== QUIC Packet Implementation Test Suite ===\n\n");

    test_long_header_initial();
    test_long_header_with_token();
    test_short_header();
    test_long_header_serialization();
    test_short_header_serialization();
    test_packet_number_encoding();
    test_complete_packet_parsing();
    test_validation_helpers();
    test_connection_id_helpers();
    test_packet_type_helpers();
    test_buffer_size_estimation();
    test_error_handling_insufficient_data();
    test_error_handling_invalid_fixed_bit();
    test_diagnostic_functions();
    test_randomized_stress();

    printf("\n=== All tests passed! ===\n");
    return 0;
}
