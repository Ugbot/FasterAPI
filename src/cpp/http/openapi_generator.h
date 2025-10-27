/**
 * OpenAPI Generator
 *
 * Generates OpenAPI 3.0.0 specification from route metadata.
 * All generation happens in C++ for maximum performance.
 *
 * Features:
 * - OpenAPI 3.0.0 compliant JSON output
 * - Path operations from RouteMetadata
 * - Parameter definitions (path, query, body)
 * - Request/response schemas from SchemaRegistry
 * - FastAPI-compatible output
 *
 * Performance: < 1ms to generate spec for 100 routes
 */

#pragma once

#include "route_metadata.h"
#include "schema_validator.h"
#include <string>
#include <vector>

namespace fasterapi {
namespace http {

/**
 * OpenAPI specification generator.
 *
 * Generates complete OpenAPI 3.0.0 JSON specification from:
 * - Registered routes (RouteRegistry)
 * - Registered schemas (SchemaRegistry)
 */
class OpenAPIGenerator {
public:
    /**
     * Generate complete OpenAPI 3.0.0 specification.
     *
     * @param routes All registered routes
     * @param title API title
     * @param version API version
     * @param description API description (optional)
     * @return Complete OpenAPI JSON string
     */
    static std::string generate(
        const std::vector<RouteMetadata>& routes,
        const std::string& title = "FasterAPI",
        const std::string& version = "0.1.0",
        const std::string& description = ""
    ) noexcept;

    /**
     * Generate paths section of OpenAPI spec.
     *
     * Groups routes by path and generates path items.
     *
     * @param routes All registered routes
     * @return JSON object for paths section
     */
    static std::string generate_paths(const std::vector<RouteMetadata>& routes) noexcept;

    /**
     * Generate a single path item.
     *
     * @param path_pattern Path pattern (e.g., "/users/{user_id}")
     * @param routes All routes for this path
     * @return JSON object for path item
     */
    static std::string generate_path_item(
        const std::string& path_pattern,
        const std::vector<const RouteMetadata*>& routes
    ) noexcept;

    /**
     * Generate a single operation (GET, POST, etc.).
     *
     * @param route Route metadata
     * @return JSON object for operation
     */
    static std::string generate_operation(const RouteMetadata& route) noexcept;

    /**
     * Generate parameters array for an operation.
     *
     * @param route Route metadata
     * @return JSON array of parameters
     */
    static std::string generate_parameters(const RouteMetadata& route) noexcept;

    /**
     * Generate a single parameter definition.
     *
     * @param param Parameter info
     * @return JSON object for parameter
     */
    static std::string generate_parameter(const ParameterInfo& param) noexcept;

    /**
     * Generate request body definition.
     *
     * @param schema_name Name of schema in registry
     * @param required Whether body is required
     * @return JSON object for request body
     */
    static std::string generate_request_body(
        const std::string& schema_name,
        bool required = true
    ) noexcept;

    /**
     * Generate responses section.
     *
     * @param route Route metadata
     * @return JSON object for responses
     */
    static std::string generate_responses(const RouteMetadata& route) noexcept;

    /**
     * Generate components section.
     *
     * Includes all schemas from SchemaRegistry.
     *
     * @return JSON object for components section
     */
    static std::string generate_components() noexcept;

    /**
     * Generate schema definition from Schema object.
     *
     * @param schema Schema object
     * @return JSON schema object
     */
    static std::string generate_schema_definition(const Schema& schema) noexcept;

    /**
     * Convert SchemaType to OpenAPI type string.
     *
     * @param type SchemaType enum
     * @return OpenAPI type string ("string", "integer", etc.)
     */
    static const char* schema_type_to_openapi_type(SchemaType type) noexcept;

    /**
     * Convert ParameterLocation to OpenAPI "in" value.
     *
     * @param location ParameterLocation enum
     * @return OpenAPI "in" value ("path", "query", etc.)
     */
    static const char* param_location_to_string(ParameterLocation location) noexcept;

    /**
     * Escape string for JSON (reuse from ValidationErrorFormatter).
     *
     * @param str String to escape
     * @return Escaped string
     */
    static std::string escape_json_string(const std::string& str) noexcept;
};

} // namespace http
} // namespace fasterapi
