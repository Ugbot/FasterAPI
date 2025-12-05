#include "auth.h"
#include <sstream>
#include <algorithm>
#include <cstring>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace fasterapi {
namespace mcp {
namespace security {

// ========== BearerTokenAuth ==========

BearerTokenAuth::BearerTokenAuth(const std::string& secret_token)
    : secret_token_(secret_token)
{
}

AuthResult BearerTokenAuth::authenticate(const std::string& auth_header) {
    // Expected format: "Bearer <token>"
    if (auth_header.substr(0, 7) != "Bearer ") {
        return AuthResult::fail("Invalid authorization header format");
    }

    std::string token = auth_header.substr(7);

    // Trim whitespace
    token.erase(0, token.find_first_not_of(" \t"));
    token.erase(token.find_last_not_of(" \t") + 1);

    if (token == secret_token_) {
        return AuthResult::ok("bearer-user", {"*"});
    }

    return AuthResult::fail("Invalid bearer token");
}

// ========== JWTAuth ==========

JWTAuth::JWTAuth(const Config& config)
    : config_(config)
{
}

AuthResult JWTAuth::authenticate(const std::string& auth_header) {
    // Expected format: "Bearer <jwt>"
    if (auth_header.substr(0, 7) != "Bearer ") {
        return AuthResult::fail("Invalid authorization header format");
    }

    std::string token = auth_header.substr(7);
    token.erase(0, token.find_first_not_of(" \t"));
    token.erase(token.find_last_not_of(" \t") + 1);

    // Parse JWT
    auto jwt_opt = parse_jwt(token);
    if (!jwt_opt.has_value()) {
        return AuthResult::fail("Invalid JWT format");
    }

    const auto& jwt = jwt_opt.value();

    // Verify signature
    if (!verify_signature(jwt)) {
        return AuthResult::fail("Invalid JWT signature");
    }

    // Decode payload
    std::string payload_json = base64_url_decode(jwt.payload);

    // Verify claims
    if (!verify_claims(payload_json)) {
        return AuthResult::fail("Invalid JWT claims");
    }

    // Extract user info
    return extract_user_info(payload_json);
}

std::optional<JWTAuth::JWT> JWTAuth::parse_jwt(const std::string& token) {
    // JWT format: header.payload.signature
    size_t dot1 = token.find('.');
    if (dot1 == std::string::npos) return std::nullopt;

    size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return std::nullopt;

    JWT jwt;
    jwt.header = token.substr(0, dot1);
    jwt.payload = token.substr(dot1 + 1, dot2 - dot1 - 1);
    jwt.signature = token.substr(dot2 + 1);

    return jwt;
}

bool JWTAuth::verify_signature(const JWT& jwt) {
    std::string data = jwt.header + "." + jwt.payload;
    std::string signature = base64_url_decode(jwt.signature);

    if (config_.algorithm == Algorithm::HS256) {
        std::string expected = hmac_sha256(config_.secret, data);
        return signature == expected;
    } else if (config_.algorithm == Algorithm::RS256) {
        return verify_rsa_sha256(config_.public_key_pem, data, signature);
    }

    return false;
}

bool JWTAuth::verify_claims(const std::string& payload_json) {
    // TODO: Parse JSON and verify claims
    // For now, basic implementation

    // Check issuer
    if (!config_.issuer.empty()) {
        std::string iss_search = "\"iss\":\"" + config_.issuer + "\"";
        if (payload_json.find(iss_search) == std::string::npos) {
            return false;
        }
    }

    // Check audience
    if (!config_.audience.empty()) {
        std::string aud_search = "\"aud\":\"" + config_.audience + "\"";
        if (payload_json.find(aud_search) == std::string::npos) {
            return false;
        }
    }

    // Check expiry
    if (config_.verify_expiry) {
        // TODO: Extract exp claim and verify
        // For now, assume valid
    }

    return true;
}

AuthResult JWTAuth::extract_user_info(const std::string& payload_json) {
    // TODO: Proper JSON parsing
    // For now, simple extraction

    std::string user_id = "jwt-user";
    std::vector<std::string> scopes;

    // Extract sub claim (subject/user ID)
    auto sub_pos = payload_json.find("\"sub\":\"");
    if (sub_pos != std::string::npos) {
        auto start = sub_pos + 7;
        auto end = payload_json.find("\"", start);
        if (end != std::string::npos) {
            user_id = payload_json.substr(start, end - start);
        }
    }

    // Extract scope claim
    auto scope_pos = payload_json.find("\"scope\":\"");
    if (scope_pos != std::string::npos) {
        auto start = scope_pos + 9;
        auto end = payload_json.find("\"", start);
        if (end != std::string::npos) {
            std::string scope_str = payload_json.substr(start, end - start);
            // Split by space
            std::istringstream iss(scope_str);
            std::string scope;
            while (iss >> scope) {
                scopes.push_back(scope);
            }
        }
    }

    return AuthResult::ok(user_id, scopes);
}

std::string JWTAuth::base64_url_decode(const std::string& input) {
    // Convert base64url to base64
    std::string base64 = input;
    std::replace(base64.begin(), base64.end(), '-', '+');
    std::replace(base64.begin(), base64.end(), '_', '/');

    // Add padding
    while (base64.length() % 4 != 0) {
        base64 += '=';
    }

    // Decode base64
    BIO* bio = BIO_new_mem_buf(base64.c_str(), base64.length());
    BIO* b64 = BIO_new(BIO_f_base64());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

    std::string output;
    output.resize(base64.length());

    int decoded_length = BIO_read(bio, &output[0], base64.length());
    output.resize(decoded_length);

    BIO_free_all(bio);

    return output;
}

std::string JWTAuth::hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    HMAC(EVP_sha256(),
         key.c_str(), key.length(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         hash, &hash_len);

    return std::string(reinterpret_cast<char*>(hash), hash_len);
}

bool JWTAuth::verify_rsa_sha256(const std::string& public_key_pem,
                                 const std::string& data,
                                 const std::string& signature) {
    // Load public key
    BIO* bio = BIO_new_mem_buf(public_key_pem.c_str(), public_key_pem.length());
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!pkey) return false;

    // Create verification context
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestVerifyUpdate(ctx, data.c_str(), data.length());

    int result = EVP_DigestVerifyFinal(ctx,
        reinterpret_cast<const unsigned char*>(signature.c_str()),
        signature.length());

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    return result == 1;
}

// ========== MultiAuth ==========

void MultiAuth::add_authenticator(const std::string& name, std::shared_ptr<Authenticator> auth) {
    authenticators_.emplace_back(name, auth);
}

AuthResult MultiAuth::authenticate(const std::string& auth_header) {
    for (const auto& [name, auth] : authenticators_) {
        auto result = auth->authenticate(auth_header);
        if (result.success) {
            return result;
        }
    }

    return AuthResult::fail("No authenticator succeeded");
}

// ========== AuthMiddleware ==========

AuthMiddleware::AuthMiddleware(std::shared_ptr<Authenticator> authenticator)
    : authenticator_(authenticator)
{
}

AuthResult AuthMiddleware::check_auth(const std::string& auth_header) {
    return authenticator_->authenticate(auth_header);
}

bool AuthMiddleware::check_tool_access(const std::vector<std::string>& user_scopes, const std::string& tool_name) {
    auto it = tool_scopes_.find(tool_name);
    if (it == tool_scopes_.end()) {
        return true;  // No scope required
    }

    return authenticator_->authorize(user_scopes, it->second);
}

void AuthMiddleware::set_tool_scope(const std::string& tool_name, const std::string& scope) {
    tool_scopes_[tool_name] = scope;
}

} // namespace security
} // namespace mcp
} // namespace fasterapi
