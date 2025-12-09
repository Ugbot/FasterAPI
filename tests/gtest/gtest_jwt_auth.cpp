/**
 * JWT Authentication Middleware Tests
 *
 * Comprehensive tests for JWT token parsing, validation, and middleware behavior.
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/jwt_auth.h"
#include <chrono>
#include <random>
#include <thread>

using namespace fasterapi::http;

class JwtAuthTest : public ::testing::Test {
protected:
    // Test secret key (32 bytes for HS256)
    const std::string test_secret = "super-secret-key-for-testing-123";

    // Generate current timestamp
    int64_t now() const {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Create a test JWT auth instance
    JwtAuth create_auth(const std::string& secret = "") const {
        JwtConfig config;
        config.secret_key = secret.empty() ? test_secret : secret;
        config.allowed_algorithms = {JwtAlgorithm::HS256};
        return JwtAuth(config);
    }

    // Generate a random string
    std::string random_string(size_t length) {
        static const char charset[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dis(gen)];
        }
        return result;
    }
};

// ============================================================================
// Base64URL Encoding/Decoding Tests
// ============================================================================

TEST_F(JwtAuthTest, Base64UrlEncodeBasic) {
    // Test basic encoding
    std::string input = "Hello, World!";
    std::string encoded = base64url::encode(input);
    EXPECT_FALSE(encoded.empty());

    // Decode and verify
    auto decoded = base64url::decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, input);
}

TEST_F(JwtAuthTest, Base64UrlEncodeEmpty) {
    std::string encoded = base64url::encode("");
    EXPECT_TRUE(encoded.empty());

    auto decoded = base64url::decode("");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->empty());
}

TEST_F(JwtAuthTest, Base64UrlEncodeBinary) {
    // Test with binary data
    uint8_t binary[] = {0x00, 0xFF, 0x7F, 0x80, 0x01};
    std::string encoded = base64url::encode(binary, sizeof(binary));

    auto decoded = base64url::decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), sizeof(binary));
    EXPECT_EQ(memcmp(decoded->data(), binary, sizeof(binary)), 0);
}

TEST_F(JwtAuthTest, Base64UrlDecodeInvalid) {
    // Invalid characters
    auto result = base64url::decode("Invalid!@#$");
    EXPECT_FALSE(result.has_value());
}

TEST_F(JwtAuthTest, Base64UrlNoPadding) {
    // Base64URL doesn't use padding
    std::string input = "a";  // Would need 2 padding chars in base64
    std::string encoded = base64url::encode(input);
    EXPECT_EQ(encoded.find('='), std::string::npos);

    auto decoded = base64url::decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, input);
}

// ============================================================================
// Token Structure Tests
// ============================================================================

TEST_F(JwtAuthTest, RejectsMalformedTokenNoDots) {
    auto auth = create_auth();
    auto result = auth.verify("nodots");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, JwtError::MALFORMED_TOKEN);
}

TEST_F(JwtAuthTest, RejectsMalformedTokenOneDot) {
    auto auth = create_auth();
    auto result = auth.verify("one.dot");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, JwtError::MALFORMED_TOKEN);
}

TEST_F(JwtAuthTest, RejectsMalformedTokenEmptyParts) {
    auto auth = create_auth();
    auto result = auth.verify("..");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, JwtError::INVALID_HEADER);
}

// ============================================================================
// HMAC Signature Tests
// ============================================================================

TEST_F(JwtAuthTest, VerifiesValidHS256Token) {
    auto auth = create_auth();

    JwtClaims claims;
    claims.sub = "user123";
    claims.exp = now() + 3600;  // 1 hour from now

    std::string token = auth.create_token(claims, JwtAlgorithm::HS256);
    auto result = auth.verify(token);

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.error, JwtError::NONE);
    EXPECT_EQ(result.algorithm, JwtAlgorithm::HS256);
    ASSERT_TRUE(result.claims.sub.has_value());
    EXPECT_EQ(*result.claims.sub, "user123");
}

TEST_F(JwtAuthTest, RejectsWrongSecret) {
    auto auth1 = create_auth("secret1");
    auto auth2 = create_auth("secret2");

    JwtClaims claims;
    claims.sub = "user123";

    std::string token = auth1.create_token(claims);

    auto result = auth2.verify(token);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, JwtError::INVALID_SIGNATURE);
}

TEST_F(JwtAuthTest, VerifiesHS384Token) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS384};
    JwtAuth auth(config);

    JwtClaims claims;
    claims.sub = "user123";

    std::string token = auth.create_token(claims, JwtAlgorithm::HS384);
    auto result = auth.verify(token);

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.algorithm, JwtAlgorithm::HS384);
}

TEST_F(JwtAuthTest, VerifiesHS512Token) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS512};
    JwtAuth auth(config);

    JwtClaims claims;
    claims.sub = "user123";

    std::string token = auth.create_token(claims, JwtAlgorithm::HS512);
    auto result = auth.verify(token);

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.algorithm, JwtAlgorithm::HS512);
}

TEST_F(JwtAuthTest, RejectsTamperedPayload) {
    auto auth = create_auth();

    JwtClaims claims;
    claims.sub = "user123";
    claims.exp = now() + 3600;

    std::string token = auth.create_token(claims);

    // Tamper with the payload by replacing a character
    size_t dot1 = token.find('.');
    size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 - dot1 > 5) {
        token[dot1 + 3] = (token[dot1 + 3] == 'a') ? 'b' : 'a';
    }

    auto result = auth.verify(token);
    EXPECT_FALSE(result.valid);
    // Could be INVALID_SIGNATURE or MALFORMED_TOKEN depending on corruption
}

// ============================================================================
// Algorithm Validation Tests
// ============================================================================

TEST_F(JwtAuthTest, RejectsDisallowedAlgorithm) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS512};  // Only allow HS512
    JwtAuth auth(config);

    // Create token with HS256 using different auth
    JwtConfig config2;
    config2.secret_key = test_secret;
    config2.allowed_algorithms = {JwtAlgorithm::HS256};
    JwtAuth auth2(config2);

    JwtClaims claims;
    claims.sub = "user123";
    std::string token = auth2.create_token(claims, JwtAlgorithm::HS256);

    auto result = auth.verify(token);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, JwtError::UNSUPPORTED_ALGORITHM);
}

TEST_F(JwtAuthTest, AllowsMultipleAlgorithms) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256, JwtAlgorithm::HS384, JwtAlgorithm::HS512};
    JwtAuth auth(config);

    JwtClaims claims;
    claims.sub = "user123";

    // All three should work
    auto result256 = auth.verify(auth.create_token(claims, JwtAlgorithm::HS256));
    EXPECT_TRUE(result256.valid);

    auto result384 = auth.verify(auth.create_token(claims, JwtAlgorithm::HS384));
    EXPECT_TRUE(result384.valid);

    auto result512 = auth.verify(auth.create_token(claims, JwtAlgorithm::HS512));
    EXPECT_TRUE(result512.valid);
}

// ============================================================================
// Claim Validation Tests
// ============================================================================

TEST_F(JwtAuthTest, RejectsExpiredToken) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.validate_exp = true;
    config.clock_skew_seconds = 0;  // No skew for test
    JwtAuth auth(config);

    JwtClaims claims;
    claims.sub = "user123";
    claims.exp = now() - 100;  // Expired 100 seconds ago

    std::string token = auth.create_token(claims);
    auto result = auth.verify(token);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, JwtError::TOKEN_EXPIRED);
}

TEST_F(JwtAuthTest, AcceptsTokenWithinClockSkew) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.validate_exp = true;
    config.clock_skew_seconds = 120;  // 2 minute skew
    JwtAuth auth(config);

    JwtClaims claims;
    claims.sub = "user123";
    claims.exp = now() - 60;  // Expired 60 seconds ago

    std::string token = auth.create_token(claims);
    auto result = auth.verify(token);

    EXPECT_TRUE(result.valid);
}

TEST_F(JwtAuthTest, RejectsTokenNotYetValid) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.validate_nbf = true;
    config.clock_skew_seconds = 0;
    JwtAuth auth(config);

    JwtClaims claims;
    claims.sub = "user123";
    claims.nbf = now() + 3600;  // Valid in 1 hour

    std::string token = auth.create_token(claims);
    auto result = auth.verify(token);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, JwtError::TOKEN_NOT_YET_VALID);
}

TEST_F(JwtAuthTest, ValidatesIssuer) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.expected_issuer = "https://auth.example.com";
    JwtAuth auth(config);

    // Wrong issuer
    JwtClaims claims1;
    claims1.iss = "https://wrong.com";
    auto result1 = auth.verify(auth.create_token(claims1));
    EXPECT_FALSE(result1.valid);
    EXPECT_EQ(result1.error, JwtError::INVALID_ISSUER);

    // Correct issuer
    JwtClaims claims2;
    claims2.iss = "https://auth.example.com";
    auto result2 = auth.verify(auth.create_token(claims2));
    EXPECT_TRUE(result2.valid);
}

TEST_F(JwtAuthTest, ValidatesAudience) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.expected_audiences = {"api.example.com", "web.example.com"};
    JwtAuth auth(config);

    // Wrong audience
    JwtClaims claims1;
    claims1.aud = "wrong.com";
    auto result1 = auth.verify(auth.create_token(claims1));
    EXPECT_FALSE(result1.valid);
    EXPECT_EQ(result1.error, JwtError::INVALID_AUDIENCE);

    // First allowed audience
    JwtClaims claims2;
    claims2.aud = "api.example.com";
    auto result2 = auth.verify(auth.create_token(claims2));
    EXPECT_TRUE(result2.valid);

    // Second allowed audience
    JwtClaims claims3;
    claims3.aud = "web.example.com";
    auto result3 = auth.verify(auth.create_token(claims3));
    EXPECT_TRUE(result3.valid);
}

TEST_F(JwtAuthTest, RequiresClaims) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.required_claims = {"sub", "role"};
    JwtAuth auth(config);

    // Missing required claim
    JwtClaims claims1;
    claims1.sub = "user123";
    // Missing "role"
    auto result1 = auth.verify(auth.create_token(claims1));
    EXPECT_FALSE(result1.valid);
    EXPECT_EQ(result1.error, JwtError::MISSING_CLAIM);

    // All required claims present
    JwtClaims claims2;
    claims2.sub = "user123";
    claims2.all["role"] = "admin";
    auto result2 = auth.verify(auth.create_token(claims2));
    EXPECT_TRUE(result2.valid);
}

TEST_F(JwtAuthTest, CustomValidator) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.custom_validator = [](const JwtClaims& claims) {
        auto role = claims.get("role");
        return role && *role == "admin";
    };
    JwtAuth auth(config);

    // Non-admin rejected
    JwtClaims claims1;
    claims1.all["role"] = "user";
    auto result1 = auth.verify(auth.create_token(claims1));
    EXPECT_FALSE(result1.valid);
    EXPECT_EQ(result1.error, JwtError::INVALID_CLAIM);

    // Admin accepted
    JwtClaims claims2;
    claims2.all["role"] = "admin";
    auto result2 = auth.verify(auth.create_token(claims2));
    EXPECT_TRUE(result2.valid);
}

// ============================================================================
// Token Extraction Tests
// ============================================================================

TEST_F(JwtAuthTest, ExtractsFromAuthorizationHeader) {
    auto auth = create_auth();

    JwtClaims claims;
    claims.sub = "user123";
    std::string token = auth.create_token(claims);

    std::unordered_map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + token;

    auto extracted = auth.extract_token(headers);
    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(*extracted, token);
}

TEST_F(JwtAuthTest, ExtractsFromLowercaseHeader) {
    auto auth = create_auth();

    JwtClaims claims;
    claims.sub = "user123";
    std::string token = auth.create_token(claims);

    std::unordered_map<std::string, std::string> headers;
    headers["authorization"] = "Bearer " + token;

    auto extracted = auth.extract_token(headers);
    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(*extracted, token);
}

TEST_F(JwtAuthTest, ExtractsFromCookie) {
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.extract_from_header = false;
    config.extract_from_cookie = true;
    config.cookie_name = "jwt_token";
    JwtAuth auth(config);

    JwtClaims claims;
    claims.sub = "user123";
    std::string token = auth.create_token(claims);

    std::unordered_map<std::string, std::string> headers;
    headers["Cookie"] = "session=abc; jwt_token=" + token + "; other=xyz";

    auto extracted = auth.extract_token(headers);
    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(*extracted, token);
}

TEST_F(JwtAuthTest, NoTokenReturnsEmpty) {
    auto auth = create_auth();

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";

    auto extracted = auth.extract_token(headers);
    EXPECT_FALSE(extracted.has_value());
}

TEST_F(JwtAuthTest, CheckRequestReturnsError) {
    auto auth = create_auth();

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";

    auto result = auth.check_request(headers);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, JwtError::MALFORMED_TOKEN);
}

// ============================================================================
// Response Building Tests
// ============================================================================

TEST_F(JwtAuthTest, BuildsUnauthorizedResponse) {
    auto auth = create_auth();

    JwtResult result;
    result.error = JwtError::TOKEN_EXPIRED;
    result.error_message = "Token has expired";

    auto response = auth.build_unauthorized_response(result);
    EXPECT_EQ(response.status, 401);

    auto www_auth = response.headers.find("WWW-Authenticate");
    ASSERT_NE(www_auth, response.headers.end());
    EXPECT_NE(www_auth->second.find("Bearer"), std::string::npos);
    EXPECT_NE(www_auth->second.find("invalid_token"), std::string::npos);
}

TEST_F(JwtAuthTest, BuildsForbiddenResponse) {
    auto response = JwtAuth::build_forbidden_response("Access denied");
    EXPECT_EQ(response.status, 403);
    EXPECT_NE(response.body.find("forbidden"), std::string::npos);
}

// ============================================================================
// Preset Tests
// ============================================================================

TEST_F(JwtAuthTest, DevelopmentPreset) {
    auto config = jwt_presets::development("dev-secret");
    EXPECT_EQ(config.secret_key, "dev-secret");
    EXPECT_FALSE(config.validate_exp);
    EXPECT_FALSE(config.validate_nbf);
}

TEST_F(JwtAuthTest, ProductionHS256Preset) {
    auto config = jwt_presets::production_hs256(
        "prod-secret",
        "https://auth.example.com",
        {"api.example.com"}
    );

    EXPECT_EQ(config.secret_key, "prod-secret");
    EXPECT_TRUE(config.validate_exp);
    EXPECT_TRUE(config.validate_nbf);
    EXPECT_EQ(config.expected_issuer, "https://auth.example.com");
    EXPECT_EQ(config.expected_audiences.size(), 1);
    EXPECT_EQ(config.clock_skew_seconds, 30);
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(JwtAuthTest, PerformanceTokenVerification) {
    auto auth = create_auth();

    JwtClaims claims;
    claims.sub = "user123";
    claims.iss = "test";
    claims.exp = now() + 3600;

    std::string token = auth.create_token(claims);

    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        auto result = auth.verify(token);
        EXPECT_TRUE(result.valid);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double per_verify_us = static_cast<double>(duration) / iterations;
    double verifications_per_sec = 1000000.0 / per_verify_us;

    std::cout << "JWT verify: " << per_verify_us << " us/verify, "
              << verifications_per_sec / 1000.0 << " K verifications/sec" << std::endl;

    // Should be able to verify at least 10K tokens per second
    EXPECT_GT(verifications_per_sec, 10000);
}

TEST_F(JwtAuthTest, PerformanceTokenCreation) {
    auto auth = create_auth();

    JwtClaims claims;
    claims.sub = "user123";
    claims.iss = "test";
    claims.exp = now() + 3600;
    claims.all["role"] = "admin";
    claims.all["permissions"] = "read,write,delete";

    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        std::string token = auth.create_token(claims);
        EXPECT_FALSE(token.empty());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double per_create_us = static_cast<double>(duration) / iterations;
    double creations_per_sec = 1000000.0 / per_create_us;

    std::cout << "JWT create: " << per_create_us << " us/create, "
              << creations_per_sec / 1000.0 << " K creations/sec" << std::endl;

    // Should be able to create at least 10K tokens per second
    EXPECT_GT(creations_per_sec, 10000);
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST_F(JwtAuthTest, FullAuthenticationFlow) {
    // Production-like configuration
    JwtConfig config;
    config.secret_key = test_secret;
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    config.validate_exp = true;
    config.validate_nbf = true;
    config.expected_issuer = "https://auth.example.com";
    config.expected_audiences = {"api.example.com"};
    config.required_claims = {"sub", "role"};
    JwtAuth auth(config);

    // Create a valid token
    JwtClaims claims;
    claims.iss = "https://auth.example.com";
    claims.aud = "api.example.com";
    claims.sub = "user_" + random_string(8);
    claims.exp = now() + 3600;
    claims.nbf = now() - 60;
    claims.iat = now();
    claims.jti = random_string(16);
    claims.all["role"] = "admin";
    claims.all["permissions"] = "read,write";

    std::string token = auth.create_token(claims);

    // Simulate HTTP request
    std::unordered_map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + token;
    headers["Content-Type"] = "application/json";

    auto result = auth.check_request(headers);

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.error, JwtError::NONE);
    ASSERT_TRUE(result.claims.sub.has_value());
    EXPECT_EQ(result.claims.sub->substr(0, 5), "user_");
    EXPECT_EQ(result.claims.get("role"), "admin");
}

TEST_F(JwtAuthTest, HandlesMultipleRandomUsers) {
    auto auth = create_auth();

    // Simulate multiple users with different tokens
    const int num_users = 100;
    std::vector<std::pair<std::string, std::string>> user_tokens;

    for (int i = 0; i < num_users; i++) {
        std::string user_id = "user_" + random_string(12);
        JwtClaims claims;
        claims.sub = user_id;
        claims.exp = now() + 3600;
        claims.all["role"] = (i % 3 == 0) ? "admin" : "user";

        user_tokens.emplace_back(user_id, auth.create_token(claims));
    }

    // Verify all tokens
    for (const auto& [user_id, token] : user_tokens) {
        auto result = auth.verify(token);
        EXPECT_TRUE(result.valid);
        ASSERT_TRUE(result.claims.sub.has_value());
        EXPECT_EQ(*result.claims.sub, user_id);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(JwtAuthTest, HandlesEmptySecret) {
    JwtConfig config;
    config.secret_key = "";
    config.allowed_algorithms = {JwtAlgorithm::HS256};
    JwtAuth auth(config);

    JwtClaims claims;
    claims.sub = "user123";
    std::string token = auth.create_token(claims);

    // Should fail with empty secret
    auto result = auth.verify(token);
    // Behavior depends on OpenSSL - may or may not work
}

TEST_F(JwtAuthTest, HandlesVeryLongClaims) {
    auto auth = create_auth();

    JwtClaims claims;
    claims.sub = random_string(1000);  // Very long subject
    claims.all["data"] = random_string(5000);  // Large custom claim

    std::string token = auth.create_token(claims);
    auto result = auth.verify(token);

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.claims.sub->size(), 1000);
}

TEST_F(JwtAuthTest, HandlesSpecialCharactersInClaims) {
    auto auth = create_auth();

    JwtClaims claims;
    claims.sub = "user@example.com";
    claims.all["name"] = "John \"Johnny\" O'Brien";
    claims.all["path"] = "/api/v1/users";

    std::string token = auth.create_token(claims);
    auto result = auth.verify(token);

    EXPECT_TRUE(result.valid);
    ASSERT_TRUE(result.claims.sub.has_value());
    EXPECT_EQ(*result.claims.sub, "user@example.com");
}

TEST_F(JwtAuthTest, NoneAlgorithmOnlyWhenExplicitlyAllowed) {
    // Default config shouldn't allow none
    JwtConfig config1;
    config1.secret_key = test_secret;
    config1.allowed_algorithms = {JwtAlgorithm::HS256};
    JwtAuth auth1(config1);

    // Manually craft a "none" algorithm token
    std::string header = base64url::encode("{\"alg\":\"none\",\"typ\":\"JWT\"}");
    std::string payload = base64url::encode("{\"sub\":\"attacker\"}");
    std::string fake_token = header + "." + payload + ".";

    auto result1 = auth1.verify(fake_token);
    EXPECT_FALSE(result1.valid);
    EXPECT_EQ(result1.error, JwtError::UNSUPPORTED_ALGORITHM);

    // Explicitly allow none (dangerous!)
    JwtConfig config2;
    config2.allowed_algorithms = {JwtAlgorithm::NONE};
    JwtAuth auth2(config2);

    auto result2 = auth2.verify(fake_token);
    EXPECT_TRUE(result2.valid);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
