#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace fasterapi {
namespace mcp {
namespace security {

/**
 * Authentication result
 */
struct AuthResult {
    bool success;
    std::string user_id;
    std::vector<std::string> scopes;
    std::string error_message;

    static AuthResult ok(const std::string& user_id, const std::vector<std::string>& scopes = {}) {
        return AuthResult{true, user_id, scopes, ""};
    }

    static AuthResult fail(const std::string& error) {
        return AuthResult{false, "", {}, error};
    }
};

/**
 * Abstract authenticator interface
 */
class Authenticator {
public:
    virtual ~Authenticator() = default;

    /**
     * Authenticate a request.
     *
     * @param auth_header Authorization header value
     * @return Authentication result
     */
    virtual AuthResult authenticate(const std::string& auth_header) = 0;

    /**
     * Check if user has required scope.
     *
     * @param scopes User's scopes
     * @param required_scope Required scope
     * @return true if authorized
     */
    virtual bool authorize(const std::vector<std::string>& scopes, const std::string& required_scope) {
        for (const auto& scope : scopes) {
            if (scope == required_scope || scope == "*") {
                return true;
            }
        }
        return false;
    }
};

/**
 * Bearer token authenticator (simple token matching)
 */
class BearerTokenAuth : public Authenticator {
public:
    /**
     * Create bearer token authenticator.
     *
     * @param secret_token The secret token to match
     */
    explicit BearerTokenAuth(const std::string& secret_token);

    AuthResult authenticate(const std::string& auth_header) override;

private:
    std::string secret_token_;
};

/**
 * JWT (JSON Web Token) authenticator
 *
 * Supports:
 * - HS256 (HMAC-SHA256) - symmetric
 * - RS256 (RSA-SHA256) - asymmetric
 */
class JWTAuth : public Authenticator {
public:
    enum class Algorithm {
        HS256,  // HMAC-SHA256
        RS256   // RSA-SHA256
    };

    struct Config {
        Algorithm algorithm = Algorithm::HS256;
        std::string secret;          // For HS256
        std::string public_key_pem;  // For RS256
        std::string issuer;          // Expected issuer (iss claim)
        std::string audience;        // Expected audience (aud claim)
        bool verify_expiry = true;   // Verify exp claim
        uint32_t clock_skew_seconds = 60;  // Allow 60s clock skew
    };

    explicit JWTAuth(const Config& config);

    AuthResult authenticate(const std::string& auth_header) override;

private:
    Config config_;

    // JWT parts
    struct JWT {
        std::string header;
        std::string payload;
        std::string signature;
    };

    // Parse JWT token
    std::optional<JWT> parse_jwt(const std::string& token);

    // Verify signature
    bool verify_signature(const JWT& jwt);

    // Verify claims
    bool verify_claims(const std::string& payload_json);

    // Extract user info from claims
    AuthResult extract_user_info(const std::string& payload_json);

    // Base64 URL decode
    std::string base64_url_decode(const std::string& input);

    // HMAC-SHA256
    std::string hmac_sha256(const std::string& key, const std::string& data);

    // Verify RSA signature
    bool verify_rsa_sha256(const std::string& public_key_pem,
                          const std::string& data,
                          const std::string& signature);
};

/**
 * Multi-authenticator (try multiple auth methods)
 */
class MultiAuth : public Authenticator {
public:
    /**
     * Add an authenticator.
     *
     * @param name Authenticator name
     * @param auth Authenticator instance
     */
    void add_authenticator(const std::string& name, std::shared_ptr<Authenticator> auth);

    /**
     * Try all authenticators until one succeeds.
     */
    AuthResult authenticate(const std::string& auth_header) override;

private:
    std::vector<std::pair<std::string, std::shared_ptr<Authenticator>>> authenticators_;
};

/**
 * Authentication middleware for MCP server
 */
class AuthMiddleware {
public:
    explicit AuthMiddleware(std::shared_ptr<Authenticator> authenticator);

    /**
     * Check if request is authenticated.
     *
     * @param auth_header Authorization header value
     * @return Authentication result
     */
    AuthResult check_auth(const std::string& auth_header);

    /**
     * Check if user is authorized for a tool.
     *
     * @param user_scopes User's scopes
     * @param tool_name Tool name
     * @return true if authorized
     */
    bool check_tool_access(const std::vector<std::string>& user_scopes, const std::string& tool_name);

    /**
     * Set required scope for a tool.
     *
     * @param tool_name Tool name
     * @param scope Required scope
     */
    void set_tool_scope(const std::string& tool_name, const std::string& scope);

private:
    std::shared_ptr<Authenticator> authenticator_;
    std::unordered_map<std::string, std::string> tool_scopes_;
};

} // namespace security
} // namespace mcp
} // namespace fasterapi
