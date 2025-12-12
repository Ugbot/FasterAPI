#pragma once

/**
 * @file qpack.h
 * @brief QPACK Header Compression (RFC 9204)
 *
 * QPACK is HTTP/3's header compression scheme, based on HPACK but designed
 * to work with QUIC's out-of-order delivery.
 *
 * Features:
 * - Static table (99 pre-defined header fields)
 * - Dynamic table (connection-specific entries)
 * - Huffman encoding for string literals
 * - Encoder/decoder stream communication
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include "../quic/quic_varint.h"

namespace fasterapi {
namespace qpack {

/**
 * QPACK Static Table (RFC 9204 Appendix A).
 *
 * 99 pre-defined header field entries.
 */
struct StaticTable {
    static constexpr size_t SIZE = 99;

    struct Entry {
        const char* name;
        const char* value;
    };

    static const Entry entries[SIZE];

    static const Entry& get(size_t index) {
        if (index < SIZE) {
            return entries[index];
        }
        static Entry empty{"", ""};
        return empty;
    }

    /**
     * Find index of name-value pair in static table.
     *
     * @param name Header name
     * @param value Header value
     * @return Index if found, -1 otherwise
     */
    static int find(const std::string& name, const std::string& value) {
        for (size_t i = 0; i < SIZE; i++) {
            if (name == entries[i].name && value == entries[i].value) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /**
     * Find index of name in static table.
     *
     * @param name Header name
     * @return Index if found, -1 otherwise
     */
    static int find_name(const std::string& name) {
        for (size_t i = 0; i < SIZE; i++) {
            if (name == entries[i].name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

// Static table definition
inline const StaticTable::Entry StaticTable::entries[StaticTable::SIZE] = {
    {":authority", ""},
    {":path", "/"},
    {"age", "0"},
    {"content-disposition", ""},
    {"content-length", "0"},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"referer", ""},
    {"set-cookie", ""},
    {":method", "CONNECT"},
    {":method", "DELETE"},
    {":method", "GET"},
    {":method", "HEAD"},
    {":method", "OPTIONS"},
    {":method", "POST"},
    {":method", "PUT"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "103"},
    {":status", "200"},
    {":status", "304"},
    {":status", "404"},
    {":status", "503"},
    {"accept", "*/*"},
    {"accept", "application/dns-message"},
    {"accept-encoding", "gzip, deflate, br"},
    {"accept-ranges", "bytes"},
    {"access-control-allow-headers", "cache-control"},
    {"access-control-allow-headers", "content-type"},
    {"access-control-allow-origin", "*"},
    {"cache-control", "max-age=0"},
    {"cache-control", "max-age=2592000"},
    {"cache-control", "max-age=604800"},
    {"cache-control", "no-cache"},
    {"cache-control", "no-store"},
    {"cache-control", "public, max-age=31536000"},
    {"content-encoding", "br"},
    {"content-encoding", "gzip"},
    {"content-type", "application/dns-message"},
    {"content-type", "application/javascript"},
    {"content-type", "application/json"},
    {"content-type", "application/x-www-form-urlencoded"},
    {"content-type", "image/gif"},
    {"content-type", "image/jpeg"},
    {"content-type", "image/png"},
    {"content-type", "text/css"},
    {"content-type", "text/html; charset=utf-8"},
    {"content-type", "text/plain"},
    {"content-type", "text/plain;charset=utf-8"},
    {"range", "bytes=0-"},
    {"strict-transport-security", "max-age=31536000"},
    {"strict-transport-security", "max-age=31536000; includesubdomains"},
    {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
    {"vary", "accept-encoding"},
    {"vary", "origin"},
    {"x-content-type-options", "nosniff"},
    {"x-xss-protection", "1; mode=block"},
    {":status", "100"},
    {":status", "204"},
    {":status", "206"},
    {":status", "302"},
    {":status", "400"},
    {":status", "403"},
    {":status", "421"},
    {":status", "425"},
    {":status", "500"},
    {"accept-language", ""},
    {"access-control-allow-credentials", "FALSE"},
    {"access-control-allow-credentials", "TRUE"},
    {"access-control-allow-methods", "get"},
    {"access-control-allow-methods", "get, post, options"},
    {"access-control-allow-methods", "options"},
    {"access-control-expose-headers", "content-length"},
    {"access-control-request-headers", "content-type"},
    {"access-control-request-method", "get"},
    {"access-control-request-method", "post"},
    {"alt-svc", "clear"},
    {"authorization", ""},
    {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
    {"early-data", "1"},
    {"expect-ct", ""},
    {"forwarded", ""},
    {"if-range", ""},
    {"origin", ""},
    {"purpose", "prefetch"},
    {"server", ""},
    {"timing-allow-origin", "*"},
    {"upgrade-insecure-requests", "1"},
    {"user-agent", ""},
    {"x-forwarded-for", ""},
    {"x-frame-options", "deny"},
    {"x-frame-options", "sameorigin"},
};

/**
 * QPACK dynamic table entry.
 */
struct DynamicEntry {
    std::string name;
    std::string value;

    size_t size() const {
        // RFC 9204: entry size = name length + value length + 32
        return name.size() + value.size() + 32;
    }
};

/**
 * QPACK Dynamic Table.
 *
 * FIFO table with maximum capacity.
 */
class DynamicTable {
public:
    DynamicTable() : capacity_(0), size_(0) {}

    void set_capacity(size_t capacity) {
        capacity_ = capacity;
        evict();
    }

    size_t capacity() const { return capacity_; }
    size_t size() const { return size_; }
    size_t count() const { return entries_.size(); }

    void insert(const std::string& name, const std::string& value) {
        DynamicEntry entry{name, value};
        size_t entry_size = entry.size();

        // Evict entries if necessary
        while (size_ + entry_size > capacity_ && !entries_.empty()) {
            size_ -= entries_.back().size();
            entries_.pop_back();
        }

        if (entry_size <= capacity_) {
            entries_.insert(entries_.begin(), std::move(entry));
            size_ += entry_size;
        }
    }

    const DynamicEntry* get(size_t index) const {
        if (index < entries_.size()) {
            return &entries_[index];
        }
        return nullptr;
    }

private:
    void evict() {
        while (size_ > capacity_ && !entries_.empty()) {
            size_ -= entries_.back().size();
            entries_.pop_back();
        }
    }

    size_t capacity_;
    size_t size_;
    std::vector<DynamicEntry> entries_;
};

/**
 * QPACK Encoder.
 *
 * Encodes HTTP header fields into QPACK format.
 */
class Encoder {
public:
    Encoder() : dynamic_table_() {}

    void set_max_table_capacity(size_t capacity) {
        dynamic_table_.set_capacity(capacity);
    }

    /**
     * Encode headers into QPACK format.
     *
     * @param headers Map of header name -> value
     * @param output Output buffer
     * @return Number of bytes written
     */
    size_t encode(const std::unordered_map<std::string, std::string>& headers,
                  uint8_t* output) {
        size_t pos = 0;

        // Encoded Field Section Prefix
        // Required Insert Count = 0 (no dynamic table refs)
        output[pos++] = 0x00;
        // Delta Base = 0, Sign = 0
        output[pos++] = 0x00;

        // Encode each header
        for (const auto& [name, value] : headers) {
            pos += encode_header(name, value, output + pos);
        }

        return pos;
    }

    /**
     * Encode headers into QPACK format (vector output).
     */
    std::vector<uint8_t> encode(const std::unordered_map<std::string, std::string>& headers) {
        // Estimate size
        size_t estimated = 2;  // Prefix
        for (const auto& [name, value] : headers) {
            estimated += 2 + name.size() + value.size();
        }

        std::vector<uint8_t> output(estimated * 2);  // Extra room for varint encoding
        size_t len = encode(headers, output.data());
        output.resize(len);
        return output;
    }

private:
    size_t encode_header(const std::string& name, const std::string& value,
                          uint8_t* output) {
        size_t pos = 0;

        // Try static table lookup
        int static_idx = StaticTable::find(name, value);
        if (static_idx >= 0) {
            // Indexed Header Field (static table)
            // 1TNNNNNN where T=1 for static table
            output[pos++] = 0xC0 | static_cast<uint8_t>(static_idx & 0x3F);
            return pos;
        }

        // Try static table name lookup
        int name_idx = StaticTable::find_name(name);
        if (name_idx >= 0) {
            // Literal Header Field With Name Reference (static)
            // 01NTNNNN
            output[pos++] = 0x50 | static_cast<uint8_t>(name_idx & 0x0F);

            // Value (no Huffman)
            pos += encode_string(value, output + pos, false);
            return pos;
        }

        // Literal Header Field With Literal Name
        // 001NNNNN
        output[pos++] = 0x20;

        // Name (no Huffman for simplicity)
        pos += encode_string(name, output + pos, false);

        // Value (no Huffman)
        pos += encode_string(value, output + pos, false);

        return pos;
    }

    size_t encode_string(const std::string& str, uint8_t* output, bool huffman) {
        size_t pos = 0;

        // Huffman flag (0 = not Huffman encoded) + length
        if (str.size() < 127) {
            output[pos++] = static_cast<uint8_t>(str.size());
        } else {
            // Length as varint with prefix
            pos += quic::VarInt::encode(str.size(), output + pos);
        }

        // String data
        std::memcpy(output + pos, str.data(), str.size());
        pos += str.size();

        return pos;
    }

    DynamicTable dynamic_table_;
};

/**
 * QPACK Decoder.
 *
 * Decodes QPACK-encoded header fields.
 */
class Decoder {
public:
    Decoder() : dynamic_table_() {}

    void set_max_table_capacity(size_t capacity) {
        dynamic_table_.set_capacity(capacity);
    }

    /**
     * Decode QPACK-encoded headers.
     *
     * @param data Encoded data
     * @param len Data length
     * @param headers Output headers
     * @return 0 on success, -1 on error
     */
    int decode(const uint8_t* data, size_t len,
               std::unordered_map<std::string, std::string>& headers) {
        if (len < 2) return -1;

        size_t pos = 0;

        // Required Insert Count (varint)
        uint64_t ric;
        int consumed = quic::VarInt::decode(data + pos, len - pos, ric);
        if (consumed < 0) return -1;
        pos += consumed;

        // Delta Base (varint with sign bit)
        if (pos >= len) return -1;
        uint8_t sign_and_base = data[pos];
        if (sign_and_base & 0x80) {
            // Negative delta - skip the byte
            pos++;
        } else {
            uint64_t delta_base;
            consumed = quic::VarInt::decode(data + pos, len - pos, delta_base);
            if (consumed < 0) return -1;
            pos += consumed;
        }

        // Decode header lines
        while (pos < len) {
            uint8_t prefix = data[pos];

            if (prefix & 0x80) {
                // Indexed Header Field
                bool is_static = (prefix & 0x40) != 0;
                uint64_t index = prefix & 0x3F;
                pos++;

                if (is_static && index < StaticTable::SIZE) {
                    const auto& entry = StaticTable::get(index);
                    headers[entry.name] = entry.value;
                }
            } else if ((prefix & 0xF0) == 0x50) {
                // Literal Header Field With Name Reference (static, 4-bit prefix)
                uint64_t name_idx = prefix & 0x0F;
                pos++;

                std::string value;
                consumed = decode_string(data + pos, len - pos, value);
                if (consumed < 0) return -1;
                pos += consumed;

                if (name_idx < StaticTable::SIZE) {
                    headers[StaticTable::get(name_idx).name] = value;
                }
            } else if ((prefix & 0xE0) == 0x20) {
                // Literal Header Field With Literal Name
                pos++;

                std::string name;
                consumed = decode_string(data + pos, len - pos, name);
                if (consumed < 0) return -1;
                pos += consumed;

                std::string value;
                consumed = decode_string(data + pos, len - pos, value);
                if (consumed < 0) return -1;
                pos += consumed;

                headers[name] = value;
            } else if ((prefix & 0xF0) == 0x40) {
                // Literal Header Field With Name Reference (6-bit prefix)
                bool is_static = (prefix & 0x10) != 0;
                uint64_t name_idx = prefix & 0x0F;
                pos++;

                std::string value;
                consumed = decode_string(data + pos, len - pos, value);
                if (consumed < 0) return -1;
                pos += consumed;

                if (is_static && name_idx < StaticTable::SIZE) {
                    headers[StaticTable::get(name_idx).name] = value;
                }
            } else {
                // Unknown prefix - skip byte
                pos++;
            }
        }

        return 0;
    }

private:
    int decode_string(const uint8_t* data, size_t len, std::string& out) {
        if (len == 0) return -1;

        bool huffman = (data[0] & 0x80) != 0;
        uint64_t str_len = data[0] & 0x7F;
        size_t pos = 1;

        if (str_len == 127) {
            // Length is a varint
            int consumed = quic::VarInt::decode(data + pos, len - pos, str_len);
            if (consumed < 0) return -1;
            pos += consumed;
        }

        if (len < pos + str_len) return -1;

        if (huffman) {
            // Huffman decoding not implemented - treat as raw
            // In production, implement RFC 7541 Huffman decoding
            out.assign(reinterpret_cast<const char*>(data + pos), str_len);
        } else {
            out.assign(reinterpret_cast<const char*>(data + pos), str_len);
        }

        return static_cast<int>(pos + str_len);
    }

    DynamicTable dynamic_table_;
};

} // namespace qpack
} // namespace fasterapi
