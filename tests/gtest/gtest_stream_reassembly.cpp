#include <gtest/gtest.h>
#include "../../src/cpp/http/quic/stream_reassembly_buffer.h"
#include <random>
#include <vector>
#include <algorithm>

using namespace fasterapi::quic;

class StreamReassemblyTest : public ::testing::Test {
protected:
    StreamReassemblyBuffer buffer;
    std::mt19937 rng{std::random_device{}()};

    // Generate random data
    std::vector<uint8_t> generate_data(size_t len) {
        std::vector<uint8_t> data(len);
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : data) {
            b = static_cast<uint8_t>(dist(rng));
        }
        return data;
    }
};

// ============= Basic Operations =============

TEST_F(StreamReassemblyTest, InitialState) {
    EXPECT_EQ(buffer.segment_count(), 0);
    EXPECT_EQ(buffer.bytes_consumed(), 0);
    EXPECT_EQ(buffer.contiguous_available(), 0);
    EXPECT_FALSE(buffer.fin_received());
    EXPECT_FALSE(buffer.is_complete());
}

TEST_F(StreamReassemblyTest, InOrderSingleWrite) {
    auto data = generate_data(100);
    ssize_t written = buffer.write_at(0, data.data(), data.size());

    EXPECT_EQ(written, 100);
    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 100);
}

TEST_F(StreamReassemblyTest, InOrderMultipleWrites) {
    auto data1 = generate_data(100);
    auto data2 = generate_data(100);
    auto data3 = generate_data(100);

    buffer.write_at(0, data1.data(), data1.size());
    buffer.write_at(100, data2.data(), data2.size());
    buffer.write_at(200, data3.data(), data3.size());

    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 300);
}

TEST_F(StreamReassemblyTest, InOrderReadVerify) {
    auto data = generate_data(100);
    buffer.write_at(0, data.data(), data.size());

    std::vector<uint8_t> out(100);
    ssize_t read = buffer.read(out.data(), out.size());

    EXPECT_EQ(read, 100);
    EXPECT_EQ(buffer.bytes_consumed(), 100);
    EXPECT_EQ(buffer.contiguous_available(), 0);
    EXPECT_EQ(out, data);
}

// ============= Out-of-Order Handling =============

TEST_F(StreamReassemblyTest, SimpleOutOfOrder) {
    auto data1 = generate_data(100);  // offset 0-100
    auto data2 = generate_data(100);  // offset 100-200

    // Receive second packet first
    buffer.write_at(100, data2.data(), data2.size());
    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 0);  // Gap at beginning

    // Receive first packet
    buffer.write_at(0, data1.data(), data1.size());
    EXPECT_EQ(buffer.segment_count(), 1);  // Should coalesce
    EXPECT_EQ(buffer.contiguous_available(), 200);

    // Verify data
    std::vector<uint8_t> out(200);
    buffer.read(out.data(), out.size());
    EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.begin() + 100), data1);
    EXPECT_EQ(std::vector<uint8_t>(out.begin() + 100, out.end()), data2);
}

TEST_F(StreamReassemblyTest, MultipleGaps) {
    auto data1 = generate_data(100);  // offset 0-100
    auto data2 = generate_data(100);  // offset 200-300
    auto data3 = generate_data(100);  // offset 400-500

    // Create pattern: [0-100), gap, [200-300), gap, [400-500)
    buffer.write_at(0, data1.data(), data1.size());
    buffer.write_at(200, data2.data(), data2.size());
    buffer.write_at(400, data3.data(), data3.size());

    EXPECT_EQ(buffer.segment_count(), 3);
    EXPECT_EQ(buffer.contiguous_available(), 100);  // Only first segment
}

TEST_F(StreamReassemblyTest, FillGapsInOrder) {
    auto data = generate_data(500);

    // Write segments with gaps
    buffer.write_at(0, data.data(), 100);
    buffer.write_at(200, data.data() + 200, 100);
    buffer.write_at(400, data.data() + 400, 100);
    EXPECT_EQ(buffer.segment_count(), 3);

    // Fill first gap [100-200)
    buffer.write_at(100, data.data() + 100, 100);
    EXPECT_EQ(buffer.segment_count(), 2);
    EXPECT_EQ(buffer.contiguous_available(), 300);

    // Fill second gap [300-400)
    buffer.write_at(300, data.data() + 300, 100);
    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 500);
}

TEST_F(StreamReassemblyTest, ReverseOrder) {
    auto data = generate_data(500);

    // Receive all packets in reverse order
    buffer.write_at(400, data.data() + 400, 100);
    EXPECT_EQ(buffer.contiguous_available(), 0);

    buffer.write_at(300, data.data() + 300, 100);
    EXPECT_EQ(buffer.contiguous_available(), 0);

    buffer.write_at(200, data.data() + 200, 100);
    EXPECT_EQ(buffer.contiguous_available(), 0);

    buffer.write_at(100, data.data() + 100, 100);
    EXPECT_EQ(buffer.contiguous_available(), 0);

    buffer.write_at(0, data.data(), 100);
    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 500);

    // Verify data integrity
    std::vector<uint8_t> out(500);
    buffer.read(out.data(), out.size());
    EXPECT_EQ(out, data);
}

// ============= Duplicate Handling =============

TEST_F(StreamReassemblyTest, CompleteDuplicate) {
    auto data = generate_data(100);

    buffer.write_at(0, data.data(), data.size());
    ssize_t written = buffer.write_at(0, data.data(), data.size());

    EXPECT_EQ(written, 100);  // Accepted (idempotent)
    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 100);
}

TEST_F(StreamReassemblyTest, DuplicateAfterConsumption) {
    auto data = generate_data(100);

    buffer.write_at(0, data.data(), data.size());

    // Consume the data
    std::vector<uint8_t> out(100);
    buffer.read(out.data(), out.size());

    // Try to write already-consumed data
    ssize_t written = buffer.write_at(0, data.data(), data.size());
    EXPECT_EQ(written, 0);  // Ignored (already consumed)
}

// ============= Overlapping Segments =============

TEST_F(StreamReassemblyTest, PartialOverlapExtendForward) {
    auto data1 = generate_data(100);
    auto data2 = generate_data(100);

    buffer.write_at(0, data1.data(), data1.size());
    buffer.write_at(50, data2.data(), data2.size());  // Overlaps [50-100), extends to 150

    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 150);
}

TEST_F(StreamReassemblyTest, PartialOverlapWithGap) {
    auto data1 = generate_data(100);
    auto data2 = generate_data(100);
    auto data3 = generate_data(100);

    buffer.write_at(0, data1.data(), 50);      // [0-50)
    buffer.write_at(100, data2.data(), 100);   // [100-200)
    buffer.write_at(40, data3.data(), 100);    // [40-140) - overlaps both

    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 200);
}

TEST_F(StreamReassemblyTest, OverlapBridgesGap) {
    buffer.write_at(0, generate_data(100).data(), 100);     // [0-100)
    buffer.write_at(200, generate_data(100).data(), 100);   // [200-300)
    EXPECT_EQ(buffer.segment_count(), 2);

    // Bridge the gap
    buffer.write_at(80, generate_data(150).data(), 150);    // [80-230) bridges gap
    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), 300);
}

// ============= FIN Handling =============

TEST_F(StreamReassemblyTest, FinWithNoGap) {
    auto data = generate_data(100);
    buffer.write_at(0, data.data(), data.size());
    buffer.set_fin_offset(100);

    EXPECT_TRUE(buffer.fin_received());
    EXPECT_FALSE(buffer.is_complete());  // Not consumed yet

    std::vector<uint8_t> out(100);
    buffer.read(out.data(), out.size());

    EXPECT_TRUE(buffer.is_complete());
}

TEST_F(StreamReassemblyTest, FinWithGap) {
    auto data = generate_data(100);
    buffer.write_at(100, data.data(), data.size());
    buffer.set_fin_offset(200);

    EXPECT_TRUE(buffer.fin_received());
    EXPECT_FALSE(buffer.is_complete());  // Gap at [0-100)

    // Fill the gap
    buffer.write_at(0, generate_data(100).data(), 100);
    EXPECT_FALSE(buffer.is_complete());  // Not consumed yet

    std::vector<uint8_t> out(200);
    buffer.read(out.data(), out.size());
    EXPECT_TRUE(buffer.is_complete());
}

TEST_F(StreamReassemblyTest, FinArrivesFirst) {
    buffer.set_fin_offset(300);
    EXPECT_FALSE(buffer.is_complete());

    // Receive data in random order
    buffer.write_at(200, generate_data(100).data(), 100);
    buffer.write_at(100, generate_data(100).data(), 100);
    buffer.write_at(0, generate_data(100).data(), 100);

    EXPECT_FALSE(buffer.is_complete());

    std::vector<uint8_t> out(300);
    buffer.read(out.data(), out.size());
    EXPECT_TRUE(buffer.is_complete());
}

// ============= Capacity Limits =============

TEST_F(StreamReassemblyTest, BufferCapacityLimit) {
    auto data = generate_data(1000);

    // Write at offset that exceeds buffer capacity
    ssize_t written = buffer.write_at(StreamReassemblyBuffer::BUFFER_SIZE + 1000,
                                       data.data(), data.size());
    EXPECT_EQ(written, -1);  // Should fail
}

TEST_F(StreamReassemblyTest, TooManySegments) {
    // Create more gaps than allowed
    size_t offset = 0;
    for (size_t i = 0; i < StreamReassemblyBuffer::MAX_SEGMENTS + 5; i++) {
        buffer.write_at(offset, generate_data(10).data(), 10);
        offset += 100;  // 90-byte gap between each
    }

    // Should have rejected some segments due to gap limit
    EXPECT_LE(buffer.segment_count(), StreamReassemblyBuffer::MAX_SEGMENTS);
}

// ============= Random Order Stress Test =============

TEST_F(StreamReassemblyTest, RandomOrderDelivery) {
    const size_t total_size = 10000;
    const size_t chunk_size = 100;

    // Generate reference data
    auto data = generate_data(total_size);

    // Create chunk indices and shuffle them
    std::vector<size_t> chunks;
    for (size_t i = 0; i < total_size / chunk_size; i++) {
        chunks.push_back(i);
    }
    std::shuffle(chunks.begin(), chunks.end(), rng);

    // Write chunks in random order
    for (size_t chunk_idx : chunks) {
        size_t offset = chunk_idx * chunk_size;
        buffer.write_at(offset, data.data() + offset, chunk_size);
    }

    EXPECT_EQ(buffer.segment_count(), 1);
    EXPECT_EQ(buffer.contiguous_available(), total_size);

    // Verify data integrity
    std::vector<uint8_t> out(total_size);
    buffer.read(out.data(), out.size());
    EXPECT_EQ(out, data);
}

TEST_F(StreamReassemblyTest, PartialReads) {
    auto data = generate_data(1000);
    buffer.write_at(0, data.data(), data.size());

    std::vector<uint8_t> out;
    uint8_t chunk[100];

    while (buffer.contiguous_available() > 0) {
        ssize_t read = buffer.read(chunk, sizeof(chunk));
        out.insert(out.end(), chunk, chunk + read);
    }

    EXPECT_EQ(out, data);
    EXPECT_EQ(buffer.bytes_consumed(), 1000);
}

// ============= Circular Buffer Wrap-Around =============

TEST_F(StreamReassemblyTest, CircularBufferWrapAround) {
    // Write and consume data multiple times to wrap around
    for (int round = 0; round < 5; round++) {
        auto data = generate_data(20000);  // More than buffer size / 4
        buffer.write_at(buffer.bytes_consumed(), data.data(), data.size());

        std::vector<uint8_t> out(data.size());
        ssize_t read = buffer.read(out.data(), out.size());

        EXPECT_EQ(read, static_cast<ssize_t>(data.size()));
        EXPECT_EQ(out, data);
    }
}

// ============= Performance Test =============

TEST_F(StreamReassemblyTest, InOrderPerformance) {
    const size_t iterations = 10000;
    const size_t chunk_size = 100;
    auto data = generate_data(chunk_size);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < iterations; i++) {
        buffer.write_at(i * chunk_size, data.data(), chunk_size);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    std::cout << "In-order write: " << (ns / iterations) << " ns/op" << std::endl;
    EXPECT_LT(ns / iterations, 500);  // <500ns per operation
}

TEST_F(StreamReassemblyTest, ReadPerformance) {
    const size_t total_size = 1000000;  // 1MB
    auto data = generate_data(total_size);
    buffer.write_at(0, data.data(), data.size());

    std::vector<uint8_t> out(1000);

    auto start = std::chrono::high_resolution_clock::now();

    while (buffer.contiguous_available() > 0) {
        buffer.read(out.data(), out.size());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "Read throughput: " << (total_size / us) << " MB/s" << std::endl;
    EXPECT_GT(total_size / us, 100);  // >100 MB/s
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
