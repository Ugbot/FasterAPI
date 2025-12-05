/**
 * Route Metadata
 *
 * Stores complete metadata for FastAPI-compatible routes.
 * Used for parameter extraction, validation, and OpenAPI generation.
 *
 * Features:
 * - Path/query/body parameter definitions
 * - Request/response schema references
 * - OpenAPI documentation metadata
 * - Python handler reference
 */

#pragma once

#include "parameter_extractor.h"
#include "schema_validator.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

// Forward declare Python object type
struct _object;
typedef _object PyObject;

namespace fasterapi {
namespace http {

/**
 * Parameter location in HTTP request.
 * Note: Using unscoped enum for Cython compatibility.
 */
enum ParameterLocation {
    PATH,      // URL path parameter (/users/{user_id})
    QUERY,     // Query string parameter (?q=search)
    BODY,      // Request body
    HEADER,    // HTTP header
    COOKIE     // Cookie
};

/**
 * Python binding helpers: Convert between int and ParameterLocation.
 * These wrappers work around Cython's limited support for scoped enums.
 */
inline ParameterLocation param_location_from_int(int value) noexcept {
    return static_cast<ParameterLocation>(value);
}

inline int param_location_to_int(ParameterLocation loc) noexcept {
    return static_cast<int>(loc);
}

/**
 * Information about a single parameter.
 */
struct ParameterInfo {
    std::string name;
    SchemaType type;
    ParameterLocation location;
    bool required;
    std::string default_value;
    std::string description;  // For OpenAPI

    ParameterInfo() = default;
    ParameterInfo(
        std::string n,
        SchemaType t,
        ParameterLocation loc,
        bool req = true
    ) : name(std::move(n)), type(t), location(loc), required(req) {}
};

/**
 * Complete metadata for a route.
 *
 * Contains all information needed for:
 * - Parameter extraction and validation
 * - Request/response validation
 * - OpenAPI schema generation
 */
struct RouteMetadata {
    // Route identification
    std::string method;                    // GET, POST, etc.
    std::string path_pattern;              // /users/{user_id}
    CompiledRoutePattern compiled_pattern; // Pre-compiled for fast matching

    // Parameters
    std::vector<ParameterInfo> parameters;

    // Schema references (names in SchemaRegistry)
    std::string request_body_schema;
    std::string response_schema;

    // Python handler
    PyObject* handler;  // Python callable

    // OpenAPI documentation
    std::string summary;
    std::string description;
    std::vector<std::string> tags;
    std::unordered_map<int, std::string> responses;  // status code → description

    RouteMetadata() : handler(nullptr) {}

    RouteMetadata(std::string m, std::string p)
        : method(std::move(m))
        , path_pattern(p)
        , compiled_pattern(p)
        , handler(nullptr) {}

    ~RouteMetadata();

    // Disable copy (PyObject* requires careful management)
    RouteMetadata(const RouteMetadata&) = delete;
    RouteMetadata& operator=(const RouteMetadata&) = delete;

    // Enable move
    RouteMetadata(RouteMetadata&&) noexcept;
    RouteMetadata& operator=(RouteMetadata&&) noexcept;
};

/**
 * Route registry with metadata.
 *
 * Stores routes with full metadata for FastAPI compatibility.
 * Replaces simple Router with enhanced functionality.
 */
class RouteRegistry {
public:
    /**
     * Register a route with metadata.
     *
     * @param metadata Route metadata (moved)
     * @return 0 on success, non-zero on error
     */
    int register_route(RouteMetadata metadata);

    /**
     * Match a route and return metadata.
     *
     * @param method HTTP method
     * @param path Request path
     * @return Pointer to matched route, or nullptr if not found
     */
    const RouteMetadata* match(const std::string& method, const std::string& path) const noexcept;

    /**
     * Get all registered routes.
     *
     * Used for OpenAPI generation.
     */
    const std::vector<RouteMetadata>& get_all_routes() const noexcept {
        return routes_;
    }

    /**
     * Clear all routes (for testing).
     */
    void clear();

private:
    std::vector<RouteMetadata> routes_;

    // Index for fast lookup: method → routes
    std::unordered_map<std::string, std::vector<size_t>> method_index_;
};

/**
 * Route metadata builder - fluent API for constructing route metadata.
 *
 * Example:
 *   auto metadata = RouteMetadataBuilder("GET", "/users/{user_id}")
 *       .path_param("user_id", SchemaType::INTEGER)
 *       .query_param("q", SchemaType::STRING, false)
 *       .response_schema("User")
 *       .summary("Get user by ID")
 *       .build();
 */
class RouteMetadataBuilder {
public:
    RouteMetadataBuilder(std::string method, std::string path)
        : metadata_(std::move(method), std::move(path)) {}

    /**
     * Add a path parameter.
     */
    RouteMetadataBuilder& path_param(
        std::string name,
        SchemaType type,
        std::string description = ""
    ) {
        ParameterInfo param(std::move(name), type, ParameterLocation::PATH, true);
        param.description = std::move(description);
        metadata_.parameters.push_back(std::move(param));
        return *this;
    }

    /**
     * Add a query parameter.
     */
    RouteMetadataBuilder& query_param(
        std::string name,
        SchemaType type,
        bool required = false,
        std::string default_value = "",
        std::string description = ""
    ) {
        ParameterInfo param(std::move(name), type, ParameterLocation::QUERY, required);
        param.default_value = std::move(default_value);
        param.description = std::move(description);
        metadata_.parameters.push_back(std::move(param));
        return *this;
    }

    /**
     * Set request body schema.
     */
    RouteMetadataBuilder& request_schema(std::string schema_name) {
        metadata_.request_body_schema = std::move(schema_name);
        return *this;
    }

    /**
     * Set response schema.
     */
    RouteMetadataBuilder& response_schema(std::string schema_name) {
        metadata_.response_schema = std::move(schema_name);
        return *this;
    }

    /**
     * Set Python handler.
     */
    RouteMetadataBuilder& handler(PyObject* h) {
        metadata_.handler = h;
        return *this;
    }

    /**
     * Set summary (for OpenAPI).
     */
    RouteMetadataBuilder& summary(std::string s) {
        metadata_.summary = std::move(s);
        return *this;
    }

    /**
     * Set description (for OpenAPI).
     */
    RouteMetadataBuilder& description(std::string d) {
        metadata_.description = std::move(d);
        return *this;
    }

    /**
     * Add a tag (for OpenAPI).
     */
    RouteMetadataBuilder& tag(std::string t) {
        metadata_.tags.push_back(std::move(t));
        return *this;
    }

    /**
     * Build and return the metadata.
     */
    RouteMetadata build() {
        return std::move(metadata_);
    }

private:
    RouteMetadata metadata_;
};

} // namespace http
} // namespace fasterapi
