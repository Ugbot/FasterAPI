/**
 * JWT Authentication Middleware
 *
 * Production-grade JWT (JSON Web Token) authentication for APIs.
 *
 * Features:
 * - HMAC algorithms: HS256, HS384, HS512
 * - RSA algorithms: RS256, RS384, RS512 (via public key verification)
 * - Token extraction from Authorization header or cookies
 * - Standard claim validation (exp, nbf, iss, aud, iat)
 * - Custom claim validators
 * - WWW-Authenticate response headers
 * - High performance with minimal allocations
 */

#pragma once

#include "http1_connection.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <optional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <array>

// OpenSSL for cryptographic operations
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>

namespace fasterapi {
namespace http {

/**
 * JWT Algorithm types
 */
enum class JwtAlgorithm {
    NONE,       // No signature (insecure, for testing only)
    HS256,      // HMAC-SHA256
    HS384,      // HMAC-SHA384
    HS512,      // HMAC-SHA512
    RS256,      // RSA-SHA256
    RS384,      // RSA-SHA384
    RS512       // RSA-SHA512
};

/**
 * JWT validation error types
 */
enum class JwtError {
    NONE,
    MALFORMED_TOKEN,        // Token format is invalid
    INVALID_HEADER,         // Header is malformed or missing fields
    UNSUPPORTED_ALGORITHM,  // Algorithm not supported or allowed
    INVALID_SIGNATURE,      // Signature verification failed
    TOKEN_EXPIRED,          // Token has expired (exp claim)
    TOKEN_NOT_YET_VALID,    // Token not yet valid (nbf claim)
    INVALID_ISSUER,         // Issuer doesn't match (iss claim)
    INVALID_AUDIENCE,       // Audience doesn't match (aud claim)
    MISSING_CLAIM,          // Required claim is missing
    INVALID_CLAIM,          // Custom claim validation failed
    KEY_ERROR               // Key loading or format error
};

/**
 * JWT Claims structure
 */
struct JwtClaims {
    // Standard claims
    std::optional<std::string> iss;     // Issuer
    std::optional<std::string> sub;     // Subject
    std::optional<std::string> aud;     // Audience (can be array, stored as first)
    std::optional<int64_t> exp;         // Expiration time
    std::optional<int64_t> nbf;         // Not before
    std::optional<int64_t> iat;         // Issued at
    std::optional<std::string> jti;     // JWT ID

    // All claims as key-value pairs (for custom claims)
    std::unordered_map<std::string, std::string> all;

    /**
     * Get a claim value
     */
    std::optional<std::string> get(const std::string& key) const {
        auto it = all.find(key);
        if (it != all.end()) {
            return it->second;
        }
        return std::nullopt;
    }
};

/**
 * JWT validation result
 */
struct JwtResult {
    bool valid{false};
    JwtError error{JwtError::NONE};
    std::string error_message;
    JwtClaims claims;
    JwtAlgorithm algorithm{JwtAlgorithm::NONE};

    // Header parts for reference
    std::string header_raw;
    std::string payload_raw;
};

/**
 * JWT configuration
 */
struct JwtConfig {
    // Secret key for HMAC algorithms
    std::string secret_key;

    // Public key for RSA algorithms (PEM format)
    std::string public_key_pem;

    // Allowed algorithms (for security, restrict which algorithms are accepted)
    std::vector<JwtAlgorithm> allowed_algorithms = {JwtAlgorithm::HS256};

    // Token extraction
    bool extract_from_header = true;         // Authorization: Bearer <token>
    bool extract_from_cookie = false;        // Cookie: jwt=<token>
    std::string cookie_name = "jwt";
    std::string header_name = "Authorization";
    std::string header_prefix = "Bearer ";   // Prefix to strip from header value

    // Claim validation
    bool validate_exp = true;                // Validate expiration
    bool validate_nbf = true;                // Validate not-before
    int64_t clock_skew_seconds = 60;         // Allow clock skew

    // Expected values (empty = don't validate)
    std::string expected_issuer;
    std::vector<std::string> expected_audiences;

    // Required claims (validation fails if missing)
    std::vector<std::string> required_claims;

    // Custom claim validator
    std::function<bool(const JwtClaims&)> custom_validator = nullptr;

    // WWW-Authenticate realm for 401 responses
    std::string realm = "api";

    JwtConfig() = default;
};

/**
 * Base64URL encoding/decoding utilities
 */
namespace base64url {

inline constexpr char encode_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

inline constexpr int8_t decode_table[] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

/**
 * Encode bytes to base64url string
 */
inline std::string encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < len) {
        result += encode_table[(data[i] >> 2) & 0x3F];
        result += encode_table[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0F)];
        result += encode_table[((data[i + 1] & 0x0F) << 2) | ((data[i + 2] >> 6) & 0x03)];
        result += encode_table[data[i + 2] & 0x3F];
        i += 3;
    }

    if (i < len) {
        result += encode_table[(data[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            result += encode_table[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0F)];
            result += encode_table[(data[i + 1] & 0x0F) << 2];
        } else {
            result += encode_table[(data[i] & 0x03) << 4];
        }
        // No padding in base64url
    }

    return result;
}

inline std::string encode(const std::string& data) {
    return encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

/**
 * Decode base64url string to bytes
 */
inline std::optional<std::string> decode(std::string_view input) {
    // Handle padding (add if missing)
    std::string padded(input);
    while (padded.size() % 4 != 0) {
        padded += '=';
    }

    std::string result;
    result.reserve((padded.size() / 4) * 3);

    for (size_t i = 0; i < padded.size(); i += 4) {
        int8_t a = decode_table[static_cast<uint8_t>(padded[i])];
        int8_t b = decode_table[static_cast<uint8_t>(padded[i + 1])];
        int8_t c = (padded[i + 2] == '=') ? 0 : decode_table[static_cast<uint8_t>(padded[i + 2])];
        int8_t d = (padded[i + 3] == '=') ? 0 : decode_table[static_cast<uint8_t>(padded[i + 3])];

        if (a < 0 || b < 0 || (padded[i + 2] != '=' && c < 0) || (padded[i + 3] != '=' && d < 0)) {
            return std::nullopt;
        }

        result += static_cast<char>((a << 2) | (b >> 4));
        if (padded[i + 2] != '=') {
            result += static_cast<char>(((b & 0x0F) << 4) | (c >> 2));
        }
        if (padded[i + 3] != '=') {
            result += static_cast<char>(((c & 0x03) << 6) | d);
        }
    }

    return result;
}

} // namespace base64url

/**
 * Simple JSON parsing (minimal, for JWT claims only)
 * Note: This is intentionally minimal. For production, consider using simdjson.
 */
namespace json {

/**
 * Skip whitespace
 */
inline void skip_ws(std::string_view& s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) {
        s.remove_prefix(1);
    }
}

/**
 * Parse a JSON string value
 */
inline std::optional<std::string> parse_string(std::string_view& s) {
    skip_ws(s);
    if (s.empty() || s[0] != '"') return std::nullopt;
    s.remove_prefix(1);

    std::string result;
    while (!s.empty() && s[0] != '"') {
        if (s[0] == '\\' && s.size() > 1) {
            s.remove_prefix(1);
            switch (s[0]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': {
                    // Unicode escape - simplified handling
                    if (s.size() >= 5) {
                        s.remove_prefix(1);
                        // Skip \uXXXX for now (simplified)
                        s.remove_prefix(4);
                        result += '?'; // Placeholder
                        continue;
                    }
                    return std::nullopt;
                }
                default: return std::nullopt;
            }
        } else {
            result += s[0];
        }
        s.remove_prefix(1);
    }

    if (s.empty() || s[0] != '"') return std::nullopt;
    s.remove_prefix(1);
    return result;
}

/**
 * Parse a JSON number as int64_t
 */
inline std::optional<int64_t> parse_number(std::string_view& s) {
    skip_ws(s);
    if (s.empty()) return std::nullopt;

    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        s.remove_prefix(1);
    }

    if (s.empty() || s[0] < '0' || s[0] > '9') return std::nullopt;

    int64_t result = 0;
    while (!s.empty() && s[0] >= '0' && s[0] <= '9') {
        result = result * 10 + (s[0] - '0');
        s.remove_prefix(1);
    }

    // Skip decimal part if present
    if (!s.empty() && s[0] == '.') {
        s.remove_prefix(1);
        while (!s.empty() && s[0] >= '0' && s[0] <= '9') {
            s.remove_prefix(1);
        }
    }

    // Skip exponent if present
    if (!s.empty() && (s[0] == 'e' || s[0] == 'E')) {
        s.remove_prefix(1);
        if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && s[0] >= '0' && s[0] <= '9') {
            s.remove_prefix(1);
        }
    }

    return negative ? -result : result;
}

/**
 * Parse JWT claims from JSON object
 */
inline std::optional<JwtClaims> parse_claims(std::string_view json) {
    JwtClaims claims;

    skip_ws(json);
    if (json.empty() || json[0] != '{') return std::nullopt;
    json.remove_prefix(1);

    while (true) {
        skip_ws(json);
        if (json.empty()) return std::nullopt;
        if (json[0] == '}') break;

        // Parse key
        auto key = parse_string(json);
        if (!key) return std::nullopt;

        skip_ws(json);
        if (json.empty() || json[0] != ':') return std::nullopt;
        json.remove_prefix(1);
        skip_ws(json);

        // Parse value
        if (json.empty()) return std::nullopt;

        if (json[0] == '"') {
            auto value = parse_string(json);
            if (!value) return std::nullopt;

            claims.all[*key] = *value;

            // Standard claims
            if (*key == "iss") claims.iss = *value;
            else if (*key == "sub") claims.sub = *value;
            else if (*key == "aud") claims.aud = *value;
            else if (*key == "jti") claims.jti = *value;

        } else if (json[0] == '-' || (json[0] >= '0' && json[0] <= '9')) {
            std::string_view orig = json;
            auto value = parse_number(json);
            if (!value) return std::nullopt;

            // Store as string in 'all'
            claims.all[*key] = std::string(orig.data(), json.data() - orig.data());

            // Standard numeric claims
            if (*key == "exp") claims.exp = *value;
            else if (*key == "nbf") claims.nbf = *value;
            else if (*key == "iat") claims.iat = *value;

        } else if (json[0] == '[') {
            // Array - just get first string element for aud
            json.remove_prefix(1);
            skip_ws(json);
            if (!json.empty() && json[0] == '"') {
                auto first = parse_string(json);
                if (first && *key == "aud") {
                    claims.aud = *first;
                }
            }
            // Skip rest of array
            int depth = 1;
            while (!json.empty() && depth > 0) {
                if (json[0] == '[') depth++;
                else if (json[0] == ']') depth--;
                json.remove_prefix(1);
            }

        } else if (json.size() >= 4 && json.substr(0, 4) == "true") {
            json.remove_prefix(4);
            claims.all[*key] = "true";

        } else if (json.size() >= 5 && json.substr(0, 5) == "false") {
            json.remove_prefix(5);
            claims.all[*key] = "false";

        } else if (json.size() >= 4 && json.substr(0, 4) == "null") {
            json.remove_prefix(4);
            // Don't store null values

        } else if (json[0] == '{') {
            // Nested object - skip
            int depth = 1;
            json.remove_prefix(1);
            while (!json.empty() && depth > 0) {
                if (json[0] == '{') depth++;
                else if (json[0] == '}') depth--;
                json.remove_prefix(1);
            }
        } else {
            return std::nullopt;
        }

        skip_ws(json);
        if (json.empty()) return std::nullopt;
        if (json[0] == ',') {
            json.remove_prefix(1);
        } else if (json[0] != '}') {
            return std::nullopt;
        }
    }

    return claims;
}

/**
 * Parse JWT header to get algorithm
 */
inline std::optional<JwtAlgorithm> parse_header_alg(std::string_view json) {
    skip_ws(json);
    if (json.empty() || json[0] != '{') return std::nullopt;
    json.remove_prefix(1);

    while (true) {
        skip_ws(json);
        if (json.empty() || json[0] == '}') break;

        auto key = parse_string(json);
        if (!key) return std::nullopt;

        skip_ws(json);
        if (json.empty() || json[0] != ':') return std::nullopt;
        json.remove_prefix(1);
        skip_ws(json);

        if (*key == "alg") {
            auto value = parse_string(json);
            if (!value) return std::nullopt;

            if (*value == "none") return JwtAlgorithm::NONE;
            if (*value == "HS256") return JwtAlgorithm::HS256;
            if (*value == "HS384") return JwtAlgorithm::HS384;
            if (*value == "HS512") return JwtAlgorithm::HS512;
            if (*value == "RS256") return JwtAlgorithm::RS256;
            if (*value == "RS384") return JwtAlgorithm::RS384;
            if (*value == "RS512") return JwtAlgorithm::RS512;
            return std::nullopt;
        }

        // Skip other values
        if (json[0] == '"') {
            parse_string(json);
        } else {
            // Skip to next comma or end
            while (!json.empty() && json[0] != ',' && json[0] != '}') {
                json.remove_prefix(1);
            }
        }

        skip_ws(json);
        if (!json.empty() && json[0] == ',') {
            json.remove_prefix(1);
        }
    }

    return std::nullopt; // No 'alg' found
}

} // namespace json

/**
 * JWT Authentication Middleware
 */
class JwtAuth {
public:
    /**
     * Create JWT authenticator with configuration
     */
    explicit JwtAuth(const JwtConfig& config)
        : config_(config) {
        build_algorithm_set();
    }

    /**
     * Verify and decode a JWT token
     */
    JwtResult verify(const std::string& token) const noexcept {
        JwtResult result;

        // Split token into parts
        auto first_dot = token.find('.');
        if (first_dot == std::string::npos) {
            result.error = JwtError::MALFORMED_TOKEN;
            result.error_message = "Token must have at least one dot";
            return result;
        }

        auto second_dot = token.find('.', first_dot + 1);
        if (second_dot == std::string::npos) {
            result.error = JwtError::MALFORMED_TOKEN;
            result.error_message = "Token must have two dots";
            return result;
        }

        std::string_view header_b64(token.data(), first_dot);
        std::string_view payload_b64(token.data() + first_dot + 1, second_dot - first_dot - 1);
        std::string_view signature_b64(token.data() + second_dot + 1);

        // Decode header
        auto header_json = base64url::decode(header_b64);
        if (!header_json) {
            result.error = JwtError::INVALID_HEADER;
            result.error_message = "Failed to decode header";
            return result;
        }
        result.header_raw = *header_json;

        // Parse algorithm from header
        auto alg = json::parse_header_alg(*header_json);
        if (!alg) {
            result.error = JwtError::INVALID_HEADER;
            result.error_message = "Missing or invalid 'alg' in header";
            return result;
        }
        result.algorithm = *alg;

        // Check if algorithm is allowed
        if (!is_algorithm_allowed(*alg)) {
            result.error = JwtError::UNSUPPORTED_ALGORITHM;
            result.error_message = "Algorithm not allowed";
            return result;
        }

        // Decode payload
        auto payload_json = base64url::decode(payload_b64);
        if (!payload_json) {
            result.error = JwtError::MALFORMED_TOKEN;
            result.error_message = "Failed to decode payload";
            return result;
        }
        result.payload_raw = *payload_json;

        // Verify signature
        std::string_view signing_input(token.data(), second_dot);
        if (!verify_signature(signing_input, signature_b64, *alg)) {
            result.error = JwtError::INVALID_SIGNATURE;
            result.error_message = "Signature verification failed";
            return result;
        }

        // Parse claims
        auto claims = json::parse_claims(*payload_json);
        if (!claims) {
            result.error = JwtError::MALFORMED_TOKEN;
            result.error_message = "Failed to parse claims";
            return result;
        }
        result.claims = *claims;

        // Validate claims
        if (!validate_claims(result)) {
            return result;  // Error already set
        }

        result.valid = true;
        return result;
    }

    /**
     * Extract token from HTTP request headers
     */
    std::optional<std::string> extract_token(
        const std::unordered_map<std::string, std::string>& headers
    ) const noexcept {
        // Try Authorization header first
        if (config_.extract_from_header) {
            auto it = headers.find(config_.header_name);
            if (it == headers.end()) {
                // Try lowercase
                it = headers.find("authorization");
            }

            if (it != headers.end()) {
                const std::string& value = it->second;
                if (value.size() > config_.header_prefix.size() &&
                    value.compare(0, config_.header_prefix.size(), config_.header_prefix) == 0) {
                    return value.substr(config_.header_prefix.size());
                }
            }
        }

        // Try cookie
        if (config_.extract_from_cookie) {
            auto it = headers.find("Cookie");
            if (it == headers.end()) {
                it = headers.find("cookie");
            }

            if (it != headers.end()) {
                return extract_cookie(it->second, config_.cookie_name);
            }
        }

        return std::nullopt;
    }

    /**
     * Check request and return result
     */
    JwtResult check_request(
        const std::unordered_map<std::string, std::string>& headers
    ) const noexcept {
        auto token = extract_token(headers);
        if (!token) {
            JwtResult result;
            result.error = JwtError::MALFORMED_TOKEN;
            result.error_message = "No token found in request";
            return result;
        }
        return verify(*token);
    }

    /**
     * Build 401 Unauthorized response with WWW-Authenticate header
     */
    Http1Response build_unauthorized_response(
        const JwtResult& result
    ) const noexcept {
        Http1Response response;
        response.status = 401;
        response.status_message = "Unauthorized";

        std::string www_auth = "Bearer realm=\"" + config_.realm + "\"";
        if (result.error != JwtError::NONE) {
            www_auth += ", error=\"invalid_token\"";
            www_auth += ", error_description=\"" + result.error_message + "\"";
        }
        response.headers["WWW-Authenticate"] = www_auth;
        response.headers["Content-Type"] = "application/json";
        response.body = "{\"error\":\"unauthorized\",\"message\":\"" +
                        result.error_message + "\"}";

        return response;
    }

    /**
     * Build 403 Forbidden response
     */
    static Http1Response build_forbidden_response(
        const std::string& message
    ) noexcept {
        Http1Response response;
        response.status = 403;
        response.status_message = "Forbidden";
        response.headers["Content-Type"] = "application/json";
        response.body = "{\"error\":\"forbidden\",\"message\":\"" + message + "\"}";
        return response;
    }

    /**
     * Get configuration
     */
    const JwtConfig& config() const noexcept {
        return config_;
    }

    /**
     * Escape a string for JSON output
     */
    static std::string json_escape(const std::string& s) {
        std::string result;
        result.reserve(s.size() + 8);  // Extra room for escapes
        for (char c : s) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        // Control character - use \uXXXX
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        result += buf;
                    } else {
                        result += c;
                    }
            }
        }
        return result;
    }

    /**
     * Create a simple JWT token (for testing)
     * Note: For production, use a proper JWT library with more features
     */
    std::string create_token(
        const JwtClaims& claims,
        JwtAlgorithm alg = JwtAlgorithm::HS256
    ) const {
        // Build header
        std::string alg_name;
        switch (alg) {
            case JwtAlgorithm::NONE: alg_name = "none"; break;
            case JwtAlgorithm::HS256: alg_name = "HS256"; break;
            case JwtAlgorithm::HS384: alg_name = "HS384"; break;
            case JwtAlgorithm::HS512: alg_name = "HS512"; break;
            case JwtAlgorithm::RS256: alg_name = "RS256"; break;
            case JwtAlgorithm::RS384: alg_name = "RS384"; break;
            case JwtAlgorithm::RS512: alg_name = "RS512"; break;
        }
        std::string header = "{\"alg\":\"" + alg_name + "\",\"typ\":\"JWT\"}";

        // Build payload
        std::string payload = "{";
        bool first = true;

        auto add_claim = [&](const std::string& key, const std::string& value, bool is_string = true) {
            if (!first) payload += ",";
            first = false;
            payload += "\"" + json_escape(key) + "\":";
            if (is_string) payload += "\"" + json_escape(value) + "\"";
            else payload += value;
        };

        if (claims.iss) add_claim("iss", *claims.iss);
        if (claims.sub) add_claim("sub", *claims.sub);
        if (claims.aud) add_claim("aud", *claims.aud);
        if (claims.exp) add_claim("exp", std::to_string(*claims.exp), false);
        if (claims.nbf) add_claim("nbf", std::to_string(*claims.nbf), false);
        if (claims.iat) add_claim("iat", std::to_string(*claims.iat), false);
        if (claims.jti) add_claim("jti", *claims.jti);

        // Add custom claims
        for (const auto& [key, value] : claims.all) {
            if (key != "iss" && key != "sub" && key != "aud" &&
                key != "exp" && key != "nbf" && key != "iat" && key != "jti") {
                add_claim(key, value);
            }
        }

        payload += "}";

        // Encode
        std::string header_b64 = base64url::encode(header);
        std::string payload_b64 = base64url::encode(payload);
        std::string signing_input = header_b64 + "." + payload_b64;

        // Sign
        std::string signature = sign(signing_input, alg);

        return signing_input + "." + signature;
    }

private:
    JwtConfig config_;
    std::unordered_set<JwtAlgorithm> allowed_algorithms_;

    void build_algorithm_set() {
        allowed_algorithms_.clear();
        for (auto alg : config_.allowed_algorithms) {
            allowed_algorithms_.insert(alg);
        }
    }

    bool is_algorithm_allowed(JwtAlgorithm alg) const noexcept {
        return allowed_algorithms_.find(alg) != allowed_algorithms_.end();
    }

    /**
     * Verify signature for HMAC algorithms
     */
    bool verify_hmac(
        std::string_view data,
        std::string_view signature_b64,
        const EVP_MD* md
    ) const noexcept {
        if (config_.secret_key.empty()) return false;

        unsigned char hmac_result[EVP_MAX_MD_SIZE];
        unsigned int hmac_len = 0;

        HMAC(md,
             config_.secret_key.data(), static_cast<int>(config_.secret_key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(),
             hmac_result, &hmac_len);

        std::string expected = base64url::encode(hmac_result, hmac_len);

        // Constant-time comparison
        if (expected.size() != signature_b64.size()) return false;
        unsigned char result = 0;
        for (size_t i = 0; i < expected.size(); i++) {
            result |= expected[i] ^ signature_b64[i];
        }
        return result == 0;
    }

    /**
     * Verify signature for RSA algorithms
     */
    bool verify_rsa(
        std::string_view data,
        std::string_view signature_b64,
        const EVP_MD* md
    ) const noexcept {
        if (config_.public_key_pem.empty()) return false;

        // Decode signature
        auto signature = base64url::decode(signature_b64);
        if (!signature) return false;

        // Load public key
        BIO* bio = BIO_new_mem_buf(config_.public_key_pem.data(),
                                   static_cast<int>(config_.public_key_pem.size()));
        if (!bio) return false;

        EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return false;

        // Verify
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            EVP_PKEY_free(pkey);
            return false;
        }

        bool valid = false;
        if (EVP_DigestVerifyInit(ctx, nullptr, md, nullptr, pkey) == 1) {
            if (EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1) {
                valid = EVP_DigestVerifyFinal(ctx,
                    reinterpret_cast<const unsigned char*>(signature->data()),
                    signature->size()) == 1;
            }
        }

        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return valid;
    }

    /**
     * Verify signature
     */
    bool verify_signature(
        std::string_view signing_input,
        std::string_view signature_b64,
        JwtAlgorithm alg
    ) const noexcept {
        switch (alg) {
            case JwtAlgorithm::NONE:
                return signature_b64.empty();

            case JwtAlgorithm::HS256:
                return verify_hmac(signing_input, signature_b64, EVP_sha256());
            case JwtAlgorithm::HS384:
                return verify_hmac(signing_input, signature_b64, EVP_sha384());
            case JwtAlgorithm::HS512:
                return verify_hmac(signing_input, signature_b64, EVP_sha512());

            case JwtAlgorithm::RS256:
                return verify_rsa(signing_input, signature_b64, EVP_sha256());
            case JwtAlgorithm::RS384:
                return verify_rsa(signing_input, signature_b64, EVP_sha384());
            case JwtAlgorithm::RS512:
                return verify_rsa(signing_input, signature_b64, EVP_sha512());

            default:
                return false;
        }
    }

    /**
     * Sign data for token creation
     */
    std::string sign(const std::string& data, JwtAlgorithm alg) const {
        switch (alg) {
            case JwtAlgorithm::NONE:
                return "";

            case JwtAlgorithm::HS256:
            case JwtAlgorithm::HS384:
            case JwtAlgorithm::HS512: {
                const EVP_MD* md = (alg == JwtAlgorithm::HS256) ? EVP_sha256() :
                                   (alg == JwtAlgorithm::HS384) ? EVP_sha384() : EVP_sha512();
                unsigned char hmac_result[EVP_MAX_MD_SIZE];
                unsigned int hmac_len = 0;

                HMAC(md,
                     config_.secret_key.data(), static_cast<int>(config_.secret_key.size()),
                     reinterpret_cast<const unsigned char*>(data.data()), data.size(),
                     hmac_result, &hmac_len);

                return base64url::encode(hmac_result, hmac_len);
            }

            default:
                return "";  // RSA signing not implemented (needs private key)
        }
    }

    /**
     * Validate claims
     */
    bool validate_claims(JwtResult& result) const noexcept {
        const auto& claims = result.claims;
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Validate expiration
        if (config_.validate_exp && claims.exp) {
            if (*claims.exp + config_.clock_skew_seconds < now) {
                result.error = JwtError::TOKEN_EXPIRED;
                result.error_message = "Token has expired";
                return false;
            }
        }

        // Validate not-before
        if (config_.validate_nbf && claims.nbf) {
            if (*claims.nbf - config_.clock_skew_seconds > now) {
                result.error = JwtError::TOKEN_NOT_YET_VALID;
                result.error_message = "Token is not yet valid";
                return false;
            }
        }

        // Validate issuer
        if (!config_.expected_issuer.empty()) {
            if (!claims.iss || *claims.iss != config_.expected_issuer) {
                result.error = JwtError::INVALID_ISSUER;
                result.error_message = "Invalid issuer";
                return false;
            }
        }

        // Validate audience
        if (!config_.expected_audiences.empty()) {
            bool found = false;
            if (claims.aud) {
                for (const auto& expected : config_.expected_audiences) {
                    if (*claims.aud == expected) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                result.error = JwtError::INVALID_AUDIENCE;
                result.error_message = "Invalid audience";
                return false;
            }
        }

        // Check required claims
        for (const auto& required : config_.required_claims) {
            if (claims.all.find(required) == claims.all.end()) {
                result.error = JwtError::MISSING_CLAIM;
                result.error_message = "Missing required claim: " + required;
                return false;
            }
        }

        // Custom validator
        if (config_.custom_validator && !config_.custom_validator(claims)) {
            result.error = JwtError::INVALID_CLAIM;
            result.error_message = "Custom validation failed";
            return false;
        }

        return true;
    }

    /**
     * Extract cookie value from Cookie header
     */
    static std::optional<std::string> extract_cookie(
        const std::string& cookie_header,
        const std::string& cookie_name
    ) noexcept {
        size_t pos = 0;
        while (pos < cookie_header.size()) {
            // Skip whitespace
            while (pos < cookie_header.size() && cookie_header[pos] == ' ') {
                pos++;
            }

            // Find =
            size_t eq_pos = cookie_header.find('=', pos);
            if (eq_pos == std::string::npos) break;

            std::string name = cookie_header.substr(pos, eq_pos - pos);

            // Find end of value
            size_t value_start = eq_pos + 1;
            size_t value_end = cookie_header.find(';', value_start);
            if (value_end == std::string::npos) {
                value_end = cookie_header.size();
            }

            if (name == cookie_name) {
                return cookie_header.substr(value_start, value_end - value_start);
            }

            pos = value_end + 1;
        }

        return std::nullopt;
    }
};

/**
 * JWT configuration presets
 */
namespace jwt_presets {

/**
 * Development preset - accepts any HS256 token with given secret
 */
inline JwtConfig development(const std::string& secret) {
    JwtConfig config;
    config.secret_key = secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.validate_exp = false;  // Don't validate expiration in dev
    config.validate_nbf = false;
    return config;
}

/**
 * Production preset - strict validation with HS256
 */
inline JwtConfig production_hs256(
    const std::string& secret,
    const std::string& issuer,
    const std::vector<std::string>& audiences
) {
    JwtConfig config;
    config.secret_key = secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.validate_exp = true;
    config.validate_nbf = true;
    config.expected_issuer = issuer;
    config.expected_audiences = audiences;
    config.clock_skew_seconds = 30;  // Stricter in production
    return config;
}

/**
 * Production preset - RSA256 with public key
 */
inline JwtConfig production_rs256(
    const std::string& public_key_pem,
    const std::string& issuer,
    const std::vector<std::string>& audiences
) {
    JwtConfig config;
    config.public_key_pem = public_key_pem;
    config.allowed_algorithms = {JwtAlgorithm::RS256};
    config.validate_exp = true;
    config.validate_nbf = true;
    config.expected_issuer = issuer;
    config.expected_audiences = audiences;
    config.clock_skew_seconds = 30;
    return config;
}

} // namespace jwt_presets

} // namespace http
} // namespace fasterapi
