/**
 * OpenAPI Generator Implementation
 */

#include "openapi_generator.h"
#include "validation_error_formatter.h"
#include <sstream>
#include <unordered_map>

namespace fasterapi {
namespace http {

// ============================================================================
// Helper Functions
// ============================================================================

std::string OpenAPIGenerator::escape_json_string(const std::string& str) noexcept {
    // Reuse from ValidationErrorFormatter
    return ValidationErrorFormatter::escape_json_string(str);
}

const char* OpenAPIGenerator::schema_type_to_openapi_type(SchemaType type) noexcept {
    switch (type) {
        case SchemaType::STRING: return "string";
        case SchemaType::INTEGER: return "integer";
        case SchemaType::FLOAT: return "number";
        case SchemaType::BOOLEAN: return "boolean";
        case SchemaType::ARRAY: return "array";
        case SchemaType::OBJECT: return "object";
        case SchemaType::NULL_TYPE: return "null";
        case SchemaType::ANY: return "object";  // "any" isn't valid in OpenAPI
    }
    return "string";
}

const char* OpenAPIGenerator::param_location_to_string(ParameterLocation location) noexcept {
    switch (location) {
        case ParameterLocation::PATH: return "path";
        case ParameterLocation::QUERY: return "query";
        case ParameterLocation::BODY: return "body";
        case ParameterLocation::HEADER: return "header";
        case ParameterLocation::COOKIE: return "cookie";
    }
    return "query";
}

// ============================================================================
// Parameter Generation
// ============================================================================

std::string OpenAPIGenerator::generate_parameter(const ParameterInfo& param) noexcept {
    std::ostringstream json;

    json << "{";
    json << "\"name\":\"" << escape_json_string(param.name) << "\",";
    json << "\"in\":\"" << param_location_to_string(param.location) << "\",";
    json << "\"required\":" << (param.required ? "true" : "false") << ",";

    if (!param.description.empty()) {
        json << "\"description\":\"" << escape_json_string(param.description) << "\",";
    }

    json << "\"schema\":{";
    json << "\"type\":\"" << schema_type_to_openapi_type(param.type) << "\"";
    json << "}";

    json << "}";
    return json.str();
}

std::string OpenAPIGenerator::generate_parameters(const RouteMetadata& route) noexcept {
    std::ostringstream json;
    json << "[";

    bool first = true;
    for (const auto& param : route.parameters) {
        // Skip body parameters (handled separately in requestBody)
        if (param.location == ParameterLocation::BODY) {
            continue;
        }

        if (!first) {
            json << ",";
        }
        first = false;

        json << generate_parameter(param);
    }

    json << "]";
    return json.str();
}

// ============================================================================
// Request Body Generation
// ============================================================================

std::string OpenAPIGenerator::generate_request_body(
    const std::string& schema_name,
    bool required
) noexcept {
    std::ostringstream json;

    json << "{";
    json << "\"required\":" << (required ? "true" : "false") << ",";
    json << "\"content\":{";
    json << "\"application/json\":{";
    json << "\"schema\":{";
    json << "\"$ref\":\"#/components/schemas/" << escape_json_string(schema_name) << "\"";
    json << "}";
    json << "}";
    json << "}";
    json << "}";

    return json.str();
}

// ============================================================================
// Response Generation
// ============================================================================

std::string OpenAPIGenerator::generate_responses(const RouteMetadata& route) noexcept {
    std::ostringstream json;

    json << "{";

    // 200 OK response
    json << "\"200\":{";
    json << "\"description\":\"Successful Response\"";

    if (!route.response_schema.empty()) {
        json << ",\"content\":{";
        json << "\"application/json\":{";
        json << "\"schema\":{";
        json << "\"$ref\":\"#/components/schemas/" << escape_json_string(route.response_schema) << "\"";
        json << "}";
        json << "}";
        json << "}";
    }

    json << "},";

    // 422 Validation Error response (if there are parameters or request body)
    if (!route.parameters.empty() || !route.request_body_schema.empty()) {
        json << "\"422\":{";
        json << "\"description\":\"Validation Error\",";
        json << "\"content\":{";
        json << "\"application/json\":{";
        json << "\"schema\":{";
        json << "\"$ref\":\"#/components/schemas/HTTPValidationError\"";
        json << "}";
        json << "}";
        json << "}";
        json << "}";
    }

    json << "}";
    return json.str();
}

// ============================================================================
// Operation Generation
// ============================================================================

std::string OpenAPIGenerator::generate_operation(const RouteMetadata& route) noexcept {
    std::ostringstream json;

    json << "{";

    // Summary
    if (!route.summary.empty()) {
        json << "\"summary\":\"" << escape_json_string(route.summary) << "\",";
    }

    // Description
    if (!route.description.empty()) {
        json << "\"description\":\"" << escape_json_string(route.description) << "\",";
    }

    // Tags
    if (!route.tags.empty()) {
        json << "\"tags\":[";
        for (size_t i = 0; i < route.tags.size(); ++i) {
            if (i > 0) json << ",";
            json << "\"" << escape_json_string(route.tags[i]) << "\"";
        }
        json << "],";
    }

    // Parameters (path, query, header, cookie)
    auto params_json = generate_parameters(route);
    if (params_json != "[]") {
        json << "\"parameters\":" << params_json << ",";
    }

    // Request Body
    if (!route.request_body_schema.empty()) {
        json << "\"requestBody\":" << generate_request_body(route.request_body_schema, true) << ",";
    }

    // Responses
    json << "\"responses\":" << generate_responses(route);

    json << "}";
    return json.str();
}

// ============================================================================
// Path Generation
// ============================================================================

std::string OpenAPIGenerator::generate_path_item(
    const std::string& path_pattern,
    const std::vector<const RouteMetadata*>& routes
) noexcept {
    std::ostringstream json;

    json << "{";

    bool first = true;
    for (const auto* route : routes) {
        if (!first) {
            json << ",";
        }
        first = false;

        // Convert HTTP method to lowercase for OpenAPI
        std::string method_lower = route->method;
        for (char& c : method_lower) {
            c = std::tolower(c);
        }

        json << "\"" << method_lower << "\":" << generate_operation(*route);
    }

    json << "}";
    return json.str();
}

std::string OpenAPIGenerator::generate_paths(const std::vector<RouteMetadata>& routes) noexcept {
    std::ostringstream json;

    // Group routes by path
    std::unordered_map<std::string, std::vector<const RouteMetadata*>> paths_map;
    for (const auto& route : routes) {
        paths_map[route.path_pattern].push_back(&route);
    }

    json << "{";

    bool first_path = true;
    for (const auto& [path, route_list] : paths_map) {
        if (!first_path) {
            json << ",";
        }
        first_path = false;

        json << "\"" << escape_json_string(path) << "\":";
        json << generate_path_item(path, route_list);
    }

    json << "}";
    return json.str();
}

// ============================================================================
// Schema Generation
// ============================================================================

std::string OpenAPIGenerator::generate_schema_definition(const Schema& schema) noexcept {
    std::ostringstream json;

    json << "{";
    json << "\"type\":\"object\",";
    json << "\"properties\":{";

    const auto& fields = schema.get_fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) json << ",";

        const auto& field = fields[i];
        json << "\"" << escape_json_string(field.name) << "\":{";
        json << "\"type\":\"" << schema_type_to_openapi_type(field.type) << "\"";
        json << "}";
    }

    json << "},";

    // Required fields
    json << "\"required\":[";
    bool first_req = true;
    for (const auto& field : fields) {
        if (field.required) {
            if (!first_req) json << ",";
            first_req = false;
            json << "\"" << escape_json_string(field.name) << "\"";
        }
    }
    json << "]";

    json << "}";
    return json.str();
}

std::string OpenAPIGenerator::generate_components() noexcept {
    std::ostringstream json;

    json << "{";
    json << "\"schemas\":{";

    // Add HTTPValidationError schema (FastAPI standard)
    json << "\"HTTPValidationError\":{";
    json << "\"type\":\"object\",";
    json << "\"properties\":{";
    json << "\"detail\":{";
    json << "\"type\":\"array\",";
    json << "\"items\":{";
    json << "\"$ref\":\"#/components/schemas/ValidationError\"";
    json << "}";
    json << "}";
    json << "}";
    json << "},";

    // Add ValidationError schema (FastAPI standard)
    json << "\"ValidationError\":{";
    json << "\"type\":\"object\",";
    json << "\"required\":[\"loc\",\"msg\",\"type\"],";
    json << "\"properties\":{";
    json << "\"loc\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},";
    json << "\"msg\":{\"type\":\"string\"},";
    json << "\"type\":{\"type\":\"string\"}";
    json << "}";
    json << "}";

    // TODO: Add user-defined schemas from SchemaRegistry
    // For now, we'll just include the validation error schemas

    json << "}";
    json << "}";
    return json.str();
}

// ============================================================================
// Main Generation
// ============================================================================

std::string OpenAPIGenerator::generate(
    const std::vector<RouteMetadata>& routes,
    const std::string& title,
    const std::string& version,
    const std::string& description
) noexcept {
    std::ostringstream json;

    json << "{";

    // OpenAPI version
    json << "\"openapi\":\"3.0.0\",";

    // Info
    json << "\"info\":{";
    json << "\"title\":\"" << escape_json_string(title) << "\",";
    json << "\"version\":\"" << escape_json_string(version) << "\"";
    if (!description.empty()) {
        json << ",\"description\":\"" << escape_json_string(description) << "\"";
    }
    json << "},";

    // Paths
    json << "\"paths\":" << generate_paths(routes) << ",";

    // Components
    json << "\"components\":" << generate_components();

    json << "}";
    return json.str();
}

} // namespace http
} // namespace fasterapi
