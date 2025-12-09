/**
 * OpenAPI Generator Tests
 *
 * Comprehensive tests for OpenAPI 3.0.0 specification generation:
 * - Basic structure validation
 * - Path operations (all HTTP methods)
 * - Parameters (path, query, header, cookie)
 * - Request/response bodies with schemas
 * - Components section with schemas
 * - Tags and descriptions
 * - Performance benchmarks
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "src/cpp/http/openapi_generator.h"
#include "src/cpp/http/static_docs.h"
#include "src/cpp/http/route_metadata.h"
#include <random>
#include <chrono>

using namespace fasterapi::http;
using namespace testing;

// =============================================================================
// Helper Functions
// =============================================================================

// Simple JSON value extraction (for testing)
std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    auto end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

bool json_contains(const std::string& json, const std::string& substr) {
    return json.find(substr) != std::string::npos;
}

// =============================================================================
// Basic Structure Tests
// =============================================================================

class OpenAPIBasicTest : public ::testing::Test {
protected:
    std::vector<RouteMetadata> routes;
};

TEST_F(OpenAPIBasicTest, EmptyRoutesGeneratesValidSpec) {
    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"openapi\":\"3.0.0\""));
    EXPECT_TRUE(json_contains(spec, "\"info\""));
    EXPECT_TRUE(json_contains(spec, "\"paths\""));
    EXPECT_TRUE(json_contains(spec, "\"components\""));
}

TEST_F(OpenAPIBasicTest, InfoSection) {
    std::string spec = OpenAPIGenerator::generate(
        routes,
        "My API",
        "1.2.3",
        "A test API"
    );

    EXPECT_TRUE(json_contains(spec, "\"title\":\"My API\""));
    EXPECT_TRUE(json_contains(spec, "\"version\":\"1.2.3\""));
    EXPECT_TRUE(json_contains(spec, "\"description\":\"A test API\""));
}

TEST_F(OpenAPIBasicTest, DefaultInfoValues) {
    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"title\":\"FasterAPI\""));
    EXPECT_TRUE(json_contains(spec, "\"version\":\"0.1.0\""));
}

TEST_F(OpenAPIBasicTest, ComponentsIncludesValidationSchemas) {
    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"HTTPValidationError\""));
    EXPECT_TRUE(json_contains(spec, "\"ValidationError\""));
}

// =============================================================================
// Path Operations Tests
// =============================================================================

class OpenAPIPathsTest : public ::testing::Test {
protected:
    std::vector<RouteMetadata> routes;
};

TEST_F(OpenAPIPathsTest, SingleGetRoute) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/users";
    route.summary = "List users";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"/users\""));
    EXPECT_TRUE(json_contains(spec, "\"get\""));
    EXPECT_TRUE(json_contains(spec, "\"summary\":\"List users\""));
}

TEST_F(OpenAPIPathsTest, MultipleHttpMethods) {
    RouteMetadata get_route;
    get_route.method = "GET";
    get_route.path_pattern = "/items";
    get_route.summary = "Get items";
    routes.push_back(std::move(get_route));

    RouteMetadata post_route;
    post_route.method = "POST";
    post_route.path_pattern = "/items";
    post_route.summary = "Create item";
    routes.push_back(std::move(post_route));

    RouteMetadata put_route;
    put_route.method = "PUT";
    put_route.path_pattern = "/items";
    put_route.summary = "Update item";
    routes.push_back(std::move(put_route));

    RouteMetadata delete_route;
    delete_route.method = "DELETE";
    delete_route.path_pattern = "/items";
    delete_route.summary = "Delete item";
    routes.push_back(std::move(delete_route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"get\""));
    EXPECT_TRUE(json_contains(spec, "\"post\""));
    EXPECT_TRUE(json_contains(spec, "\"put\""));
    EXPECT_TRUE(json_contains(spec, "\"delete\""));
}

TEST_F(OpenAPIPathsTest, PathWithParameters) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/users/{user_id}/posts/{post_id}";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"/users/{user_id}/posts/{post_id}\""));
}

TEST_F(OpenAPIPathsTest, MultiplePaths) {
    RouteMetadata route1;
    route1.method = "GET";
    route1.path_pattern = "/users";
    routes.push_back(std::move(route1));

    RouteMetadata route2;
    route2.method = "GET";
    route2.path_pattern = "/posts";
    routes.push_back(std::move(route2));

    RouteMetadata route3;
    route3.method = "GET";
    route3.path_pattern = "/comments";
    routes.push_back(std::move(route3));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"/users\""));
    EXPECT_TRUE(json_contains(spec, "\"/posts\""));
    EXPECT_TRUE(json_contains(spec, "\"/comments\""));
}

TEST_F(OpenAPIPathsTest, RouteWithDescription) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/items";
    route.summary = "Get all items";
    route.description = "Returns a list of all items with pagination support.";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"summary\":\"Get all items\""));
    EXPECT_TRUE(json_contains(spec, "\"description\":\"Returns a list of all items with pagination support.\""));
}

TEST_F(OpenAPIPathsTest, RouteWithTags) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/users";
    route.tags = {"users", "admin"};
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"tags\":[\"users\",\"admin\"]"));
}

// =============================================================================
// Parameters Tests
// =============================================================================

class OpenAPIParametersTest : public ::testing::Test {
protected:
    std::vector<RouteMetadata> routes;
};

TEST_F(OpenAPIParametersTest, PathParameter) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/users/{user_id}";

    ParameterInfo param;
    param.name = "user_id";
    param.location = ParameterLocation::PATH;
    param.type = SchemaType::INTEGER;
    param.required = true;
    param.description = "User ID";
    route.parameters.push_back(param);

    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"name\":\"user_id\""));
    EXPECT_TRUE(json_contains(spec, "\"in\":\"path\""));
    EXPECT_TRUE(json_contains(spec, "\"required\":true"));
    EXPECT_TRUE(json_contains(spec, "\"description\":\"User ID\""));
    EXPECT_TRUE(json_contains(spec, "\"type\":\"integer\""));
}

TEST_F(OpenAPIParametersTest, QueryParameter) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/items";

    ParameterInfo param;
    param.name = "limit";
    param.location = ParameterLocation::QUERY;
    param.type = SchemaType::INTEGER;
    param.required = false;
    route.parameters.push_back(param);

    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"name\":\"limit\""));
    EXPECT_TRUE(json_contains(spec, "\"in\":\"query\""));
    EXPECT_TRUE(json_contains(spec, "\"required\":false"));
}

TEST_F(OpenAPIParametersTest, HeaderParameter) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/secure";

    ParameterInfo param;
    param.name = "X-API-Key";
    param.location = ParameterLocation::HEADER;
    param.type = SchemaType::STRING;
    param.required = true;
    route.parameters.push_back(param);

    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"name\":\"X-API-Key\""));
    EXPECT_TRUE(json_contains(spec, "\"in\":\"header\""));
}

TEST_F(OpenAPIParametersTest, CookieParameter) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/session";

    ParameterInfo param;
    param.name = "session_id";
    param.location = ParameterLocation::COOKIE;
    param.type = SchemaType::STRING;
    param.required = true;
    route.parameters.push_back(param);

    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"name\":\"session_id\""));
    EXPECT_TRUE(json_contains(spec, "\"in\":\"cookie\""));
}

TEST_F(OpenAPIParametersTest, MultipleParameters) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/search/{category}";

    ParameterInfo path_param;
    path_param.name = "category";
    path_param.location = ParameterLocation::PATH;
    path_param.type = SchemaType::STRING;
    path_param.required = true;
    route.parameters.push_back(path_param);

    ParameterInfo query_param1;
    query_param1.name = "q";
    query_param1.location = ParameterLocation::QUERY;
    query_param1.type = SchemaType::STRING;
    query_param1.required = true;
    route.parameters.push_back(query_param1);

    ParameterInfo query_param2;
    query_param2.name = "page";
    query_param2.location = ParameterLocation::QUERY;
    query_param2.type = SchemaType::INTEGER;
    query_param2.required = false;
    route.parameters.push_back(query_param2);

    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"name\":\"category\""));
    EXPECT_TRUE(json_contains(spec, "\"name\":\"q\""));
    EXPECT_TRUE(json_contains(spec, "\"name\":\"page\""));
}

TEST_F(OpenAPIParametersTest, AllSchemaTypes) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/types";

    std::vector<std::pair<SchemaType, const char*>> types = {
        {SchemaType::STRING, "string"},
        {SchemaType::INTEGER, "integer"},
        {SchemaType::FLOAT, "number"},
        {SchemaType::BOOLEAN, "boolean"},
        {SchemaType::ARRAY, "array"},
        {SchemaType::OBJECT, "object"},
    };

    int i = 0;
    for (const auto& [type, expected] : types) {
        ParameterInfo param;
        param.name = "param_" + std::to_string(i++);
        param.location = ParameterLocation::QUERY;
        param.type = type;
        route.parameters.push_back(param);
    }

    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    for (const auto& [type, expected] : types) {
        EXPECT_TRUE(json_contains(spec, std::string("\"type\":\"") + expected + "\""))
            << "Expected type: " << expected;
    }
}

// =============================================================================
// Request Body Tests
// =============================================================================

class OpenAPIRequestBodyTest : public ::testing::Test {
protected:
    std::vector<RouteMetadata> routes;
};

TEST_F(OpenAPIRequestBodyTest, PostWithRequestBody) {
    RouteMetadata route;
    route.method = "POST";
    route.path_pattern = "/users";
    route.request_body_schema = "CreateUser";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"requestBody\""));
    EXPECT_TRUE(json_contains(spec, "\"application/json\""));
    EXPECT_TRUE(json_contains(spec, "\"$ref\":\"#/components/schemas/CreateUser\""));
}

TEST_F(OpenAPIRequestBodyTest, PutWithRequestBody) {
    RouteMetadata route;
    route.method = "PUT";
    route.path_pattern = "/users/{id}";
    route.request_body_schema = "UpdateUser";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"requestBody\""));
    EXPECT_TRUE(json_contains(spec, "\"$ref\":\"#/components/schemas/UpdateUser\""));
}

// =============================================================================
// Response Tests
// =============================================================================

class OpenAPIResponseTest : public ::testing::Test {
protected:
    std::vector<RouteMetadata> routes;
};

TEST_F(OpenAPIResponseTest, DefaultSuccessResponse) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/items";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"200\""));
    EXPECT_TRUE(json_contains(spec, "\"Successful Response\""));
}

TEST_F(OpenAPIResponseTest, ResponseWithSchema) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/users/{id}";
    route.response_schema = "User";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"$ref\":\"#/components/schemas/User\""));
}

TEST_F(OpenAPIResponseTest, ValidationErrorResponse) {
    RouteMetadata route;
    route.method = "POST";
    route.path_pattern = "/users";
    route.request_body_schema = "CreateUser";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "\"422\""));
    EXPECT_TRUE(json_contains(spec, "\"Validation Error\""));
    EXPECT_TRUE(json_contains(spec, "\"$ref\":\"#/components/schemas/HTTPValidationError\""));
}

// =============================================================================
// JSON Escaping Tests
// =============================================================================

class OpenAPIEscapingTest : public ::testing::Test {
protected:
    std::vector<RouteMetadata> routes;
};

TEST_F(OpenAPIEscapingTest, EscapesQuotes) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/test";
    route.summary = "Test \"quotes\" in summary";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "Test \\\"quotes\\\" in summary"));
}

TEST_F(OpenAPIEscapingTest, EscapesBackslashes) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/test";
    route.summary = "Test\\path";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "Test\\\\path"));
}

TEST_F(OpenAPIEscapingTest, EscapesNewlines) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/test";
    route.description = "Line 1\nLine 2\rLine 3";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);

    EXPECT_TRUE(json_contains(spec, "Line 1\\nLine 2\\rLine 3"));
}

// =============================================================================
// Static Docs Tests
// =============================================================================

class StaticDocsTest : public ::testing::Test {};

TEST_F(StaticDocsTest, SwaggerUIContainsTitle) {
    std::string html = StaticDocs::generate_swagger_ui("/openapi.json", "My API");

    EXPECT_TRUE(html.find("<title>My API</title>") != std::string::npos ||
                html.find("My API") != std::string::npos);
}

TEST_F(StaticDocsTest, SwaggerUIContainsOpenAPIUrl) {
    std::string html = StaticDocs::generate_swagger_ui("/api/v1/openapi.json");

    EXPECT_TRUE(html.find("/api/v1/openapi.json") != std::string::npos);
}

TEST_F(StaticDocsTest, SwaggerUIContainsSwaggerLibrary) {
    std::string html = StaticDocs::generate_swagger_ui();

    // Should reference Swagger UI CDN
    EXPECT_TRUE(html.find("swagger-ui") != std::string::npos);
}

TEST_F(StaticDocsTest, ReDocContainsTitle) {
    std::string html = StaticDocs::generate_redoc("/openapi.json", "ReDoc API");

    EXPECT_TRUE(html.find("ReDoc API") != std::string::npos);
}

TEST_F(StaticDocsTest, ReDocContainsOpenAPIUrl) {
    std::string html = StaticDocs::generate_redoc("/custom/openapi.json");

    EXPECT_TRUE(html.find("/custom/openapi.json") != std::string::npos);
}

TEST_F(StaticDocsTest, ReDocContainsReDocLibrary) {
    std::string html = StaticDocs::generate_redoc();

    // Should reference ReDoc CDN or library
    EXPECT_TRUE(html.find("redoc") != std::string::npos ||
                html.find("Redoc") != std::string::npos);
}

TEST_F(StaticDocsTest, SwaggerUIResponseIncludesHeaders) {
    std::string response = StaticDocs::generate_swagger_ui_response();

    EXPECT_TRUE(response.find("Content-Type: text/html") != std::string::npos ||
                response.find("text/html") != std::string::npos);
}

TEST_F(StaticDocsTest, ReDocResponseIncludesHeaders) {
    std::string response = StaticDocs::generate_redoc_response();

    EXPECT_TRUE(response.find("Content-Type: text/html") != std::string::npos ||
                response.find("text/html") != std::string::npos);
}

// =============================================================================
// Performance Tests
// =============================================================================

class OpenAPIPerformanceTest : public ::testing::Test {
protected:
    std::vector<RouteMetadata> generate_routes(int count) {
        std::vector<RouteMetadata> routes;
        routes.reserve(count);

        std::vector<std::string> methods = {"GET", "POST", "PUT", "DELETE", "PATCH"};
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> method_dis(0, methods.size() - 1);

        for (int i = 0; i < count; ++i) {
            RouteMetadata route;
            route.method = methods[method_dis(gen)];
            route.path_pattern = "/api/v1/resource_" + std::to_string(i) + "/{id}";
            route.summary = "Operation for resource " + std::to_string(i);
            route.description = "This is a detailed description for the operation.";
            route.tags = {"api", "v1"};

            // Add path parameter
            ParameterInfo path_param;
            path_param.name = "id";
            path_param.location = ParameterLocation::PATH;
            path_param.type = SchemaType::INTEGER;
            path_param.required = true;
            route.parameters.push_back(path_param);

            // Add query parameters
            ParameterInfo query_param1;
            query_param1.name = "page";
            query_param1.location = ParameterLocation::QUERY;
            query_param1.type = SchemaType::INTEGER;
            query_param1.required = false;
            route.parameters.push_back(query_param1);

            ParameterInfo query_param2;
            query_param2.name = "limit";
            query_param2.location = ParameterLocation::QUERY;
            query_param2.type = SchemaType::INTEGER;
            query_param2.required = false;
            route.parameters.push_back(query_param2);

            if (route.method == "POST" || route.method == "PUT" || route.method == "PATCH") {
                route.request_body_schema = "Resource" + std::to_string(i);
            }

            route.response_schema = "ResourceResponse";

            routes.push_back(std::move(route));
        }

        return routes;
    }
};

TEST_F(OpenAPIPerformanceTest, GenerateSpec100Routes) {
    auto routes = generate_routes(100);

    auto start = std::chrono::steady_clock::now();

    std::string spec = OpenAPIGenerator::generate(routes);

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "100 routes: " << duration.count() << " us, "
              << spec.size() << " bytes" << std::endl;

    // Should be under 1ms as documented
    EXPECT_LT(duration.count(), 1000);

    // Verify it's valid
    EXPECT_TRUE(json_contains(spec, "\"openapi\":\"3.0.0\""));
}

TEST_F(OpenAPIPerformanceTest, GenerateSpec500Routes) {
    auto routes = generate_routes(500);

    auto start = std::chrono::steady_clock::now();

    std::string spec = OpenAPIGenerator::generate(routes);

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "500 routes: " << duration.count() << " us, "
              << spec.size() << " bytes" << std::endl;

    // Should be under 5ms for 500 routes
    EXPECT_LT(duration.count(), 5000);
}

TEST_F(OpenAPIPerformanceTest, ThroughputTest) {
    auto routes = generate_routes(50);
    const int iterations = 1000;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
        std::string spec = OpenAPIGenerator::generate(routes);
        (void)spec.size();  // Prevent optimization
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double specs_per_sec = (iterations * 1000.0) / duration.count();

    std::cout << "OpenAPI generation throughput: " << specs_per_sec << " specs/sec" << std::endl;

    // Should generate at least 1000 specs/sec
    EXPECT_GT(specs_per_sec, 1000.0);
}

// =============================================================================
// Edge Cases Tests
// =============================================================================

class OpenAPIEdgeCasesTest : public ::testing::Test {
protected:
    std::vector<RouteMetadata> routes;
};

TEST_F(OpenAPIEdgeCasesTest, EmptyPathPattern) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "";
    routes.push_back(std::move(route));

    // Should not crash
    std::string spec = OpenAPIGenerator::generate(routes);
    EXPECT_TRUE(json_contains(spec, "\"openapi\":\"3.0.0\""));
}

TEST_F(OpenAPIEdgeCasesTest, VeryLongPath) {
    RouteMetadata route;
    route.method = "GET";
    std::string long_path = "/api/v1/very/long/path/with/many/segments/that/goes/on/forever";
    route.path_pattern = long_path;
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);
    EXPECT_TRUE(json_contains(spec, long_path));
}

TEST_F(OpenAPIEdgeCasesTest, SpecialCharactersInPath) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/api/items/{item-id}";
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);
    EXPECT_TRUE(json_contains(spec, "/api/items/{item-id}"));
}

TEST_F(OpenAPIEdgeCasesTest, UnicodeInSummary) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/test";
    route.summary = "Get items \xE2\x9C\x93";  // UTF-8 checkmark
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);
    EXPECT_TRUE(spec.size() > 0);
}

TEST_F(OpenAPIEdgeCasesTest, ManyTags) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/test";
    for (int i = 0; i < 20; ++i) {
        route.tags.push_back("tag_" + std::to_string(i));
    }
    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);
    EXPECT_TRUE(json_contains(spec, "tag_0"));
    EXPECT_TRUE(json_contains(spec, "tag_19"));
}

TEST_F(OpenAPIEdgeCasesTest, ManyParameters) {
    RouteMetadata route;
    route.method = "GET";
    route.path_pattern = "/search";

    for (int i = 0; i < 50; ++i) {
        ParameterInfo param;
        param.name = "param_" + std::to_string(i);
        param.location = ParameterLocation::QUERY;
        param.type = SchemaType::STRING;
        param.required = false;
        route.parameters.push_back(param);
    }

    routes.push_back(std::move(route));

    std::string spec = OpenAPIGenerator::generate(routes);
    EXPECT_TRUE(json_contains(spec, "param_0"));
    EXPECT_TRUE(json_contains(spec, "param_49"));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
