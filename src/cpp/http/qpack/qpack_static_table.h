#pragma once

#include <string_view>
#include <cstdint>

namespace fasterapi {
namespace qpack {

/**
 * QPACK static table entry.
 */
struct StaticEntry {
    std::string_view name;
    std::string_view value;
};

/**
 * QPACK Static Table (RFC 9204 Appendix A).
 * 
 * Contains 99 predefined entries for common HTTP headers.
 * Indices 0-98 (but indexing starts at 0 in code, 0 in spec).
 */
class QPACKStaticTable {
public:
    /**
     * Get static table size.
     */
    static constexpr size_t size() noexcept { return 99; }
    
    /**
     * Get entry by index.
     * 
     * @param index Index (0-98)
     * @return Entry or nullptr if out of range
     */
    static const StaticEntry* get(size_t index) noexcept {
        if (index >= size()) return nullptr;
        return &entries_[index];
    }
    
    /**
     * Find entry by name and value.
     * 
     * @param name Header name
     * @param value Header value
     * @return Index if found, -1 otherwise
     */
    static int find(std::string_view name, std::string_view value) noexcept {
        for (size_t i = 0; i < size(); i++) {
            if (entries_[i].name == name && entries_[i].value == value) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    
    /**
     * Find entry by name only (returns first match).
     * 
     * @param name Header name
     * @return Index if found, -1 otherwise
     */
    static int find_name(std::string_view name) noexcept {
        for (size_t i = 0; i < size(); i++) {
            if (entries_[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

private:
    // QPACK Static Table (RFC 9204 Appendix A)
    // Full 99 entries
    static constexpr StaticEntry entries_[] = {
        {":authority", ""},                          // 0
        {":path", "/"},                              // 1
        {"age", "0"},                                // 2
        {"content-disposition", ""},                 // 3
        {"content-length", "0"},                     // 4
        {"cookie", ""},                              // 5
        {"date", ""},                                // 6
        {"etag", ""},                                // 7
        {"if-modified-since", ""},                   // 8
        {"if-none-match", ""},                       // 9
        {"last-modified", ""},                       // 10
        {"link", ""},                                // 11
        {"location", ""},                            // 12
        {"referer", ""},                             // 13
        {"set-cookie", ""},                          // 14
        {":method", "CONNECT"},                      // 15
        {":method", "DELETE"},                       // 16
        {":method", "GET"},                          // 17
        {":method", "HEAD"},                         // 18
        {":method", "OPTIONS"},                      // 19
        {":method", "POST"},                         // 20
        {":method", "PUT"},                          // 21
        {":scheme", "http"},                         // 22
        {":scheme", "https"},                        // 23
        {":status", "103"},                          // 24
        {":status", "200"},                          // 25
        {":status", "304"},                          // 26
        {":status", "404"},                          // 27
        {":status", "503"},                          // 28
        {"accept", "*/*"},                           // 29
        {"accept", "application/dns-message"},       // 30
        {"accept-encoding", "gzip, deflate, br"},    // 31
        {"accept-ranges", "bytes"},                  // 32
        {"access-control-allow-headers", "cache-control"}, // 33
        {"access-control-allow-headers", "content-type"}, // 34
        {"access-control-allow-origin", "*"},        // 35
        {"cache-control", "max-age=0"},              // 36
        {"cache-control", "max-age=2592000"},        // 37
        {"cache-control", "max-age=604800"},         // 38
        {"cache-control", "no-cache"},               // 39
        {"cache-control", "no-store"},               // 40
        {"cache-control", "public, max-age=31536000"}, // 41
        {"content-encoding", "br"},                  // 42
        {"content-encoding", "gzip"},                // 43
        {"content-type", "application/dns-message"}, // 44
        {"content-type", "application/javascript"},  // 45
        {"content-type", "application/json"},        // 46
        {"content-type", "application/x-www-form-urlencoded"}, // 47
        {"content-type", "image/gif"},               // 48
        {"content-type", "image/jpeg"},              // 49
        {"content-type", "image/png"},               // 50
        {"content-type", "text/css"},                // 51
        {"content-type", "text/html; charset=utf-8"}, // 52
        {"content-type", "text/plain"},              // 53
        {"content-type", "text/plain;charset=utf-8"}, // 54
        {"range", "bytes=0-"},                       // 55
        {"strict-transport-security", "max-age=31536000"}, // 56
        {"strict-transport-security", "max-age=31536000; includesubdomains"}, // 57
        {"strict-transport-security", "max-age=31536000; includesubdomains; preload"}, // 58
        {"vary", "accept-encoding"},                 // 59
        {"vary", "origin"},                          // 60
        {"x-content-type-options", "nosniff"},       // 61
        {"x-xss-protection", "1; mode=block"},       // 62
        {":status", "100"},                          // 63
        {":status", "204"},                          // 64
        {":status", "206"},                          // 65
        {":status", "302"},                          // 66
        {":status", "400"},                          // 67
        {":status", "403"},                          // 68
        {":status", "421"},                          // 69
        {":status", "425"},                          // 70
        {":status", "500"},                          // 71
        {"accept-language", ""},                     // 72
        {"access-control-allow-credentials", "FALSE"}, // 73
        {"access-control-allow-credentials", "TRUE"}, // 74
        {"access-control-allow-headers", "*"},       // 75
        {"access-control-allow-methods", "get"},     // 76
        {"access-control-allow-methods", "get, post, options"}, // 77
        {"access-control-allow-methods", "options"}, // 78
        {"access-control-expose-headers", "content-length"}, // 79
        {"access-control-request-headers", "content-type"}, // 80
        {"access-control-request-method", "get"},    // 81
        {"access-control-request-method", "post"},   // 82
        {"alt-svc", "clear"},                        // 83
        {"authorization", ""},                       // 84
        {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"}, // 85
        {"early-data", "1"},                         // 86
        {"expect-ct", ""},                           // 87
        {"forwarded", ""},                           // 88
        {"if-range", ""},                            // 89
        {"origin", ""},                              // 90
        {"purpose", "prefetch"},                     // 91
        {"server", ""},                              // 92
        {"timing-allow-origin", "*"},                // 93
        {"upgrade-insecure-requests", "1"},          // 94
        {"user-agent", ""},                          // 95
        {"x-forwarded-for", ""},                     // 96
        {"x-frame-options", "deny"},                 // 97
        {"x-frame-options", "sameorigin"},           // 98
    };
};

// Static table definition
constexpr StaticEntry QPACKStaticTable::entries_[];

} // namespace qpack
} // namespace fasterapi
