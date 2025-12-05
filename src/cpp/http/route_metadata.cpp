/**
 * Route Metadata Implementation
 */

#include "route_metadata.h"
#include "core/logger.h"
#include <Python.h>

namespace fasterapi {
namespace http {

// ============================================================================
// RouteMetadata Implementation
// ============================================================================

RouteMetadata::~RouteMetadata() {
    // Decrement Python reference count if we have a handler
    if (handler != nullptr) {
        Py_DECREF(handler);
        handler = nullptr;
    }
}

RouteMetadata::RouteMetadata(RouteMetadata&& other) noexcept
    : method(std::move(other.method))
    , path_pattern(std::move(other.path_pattern))
    , compiled_pattern(std::move(other.compiled_pattern))
    , parameters(std::move(other.parameters))
    , request_body_schema(std::move(other.request_body_schema))
    , response_schema(std::move(other.response_schema))
    , handler(other.handler)
    , summary(std::move(other.summary))
    , description(std::move(other.description))
    , tags(std::move(other.tags))
    , responses(std::move(other.responses))
{
    // Transfer ownership of PyObject* - don't change refcount
    other.handler = nullptr;
}

RouteMetadata& RouteMetadata::operator=(RouteMetadata&& other) noexcept {
    if (this != &other) {
        // Release current handler
        if (handler != nullptr) {
            Py_DECREF(handler);
        }

        // Move all fields
        method = std::move(other.method);
        path_pattern = std::move(other.path_pattern);
        compiled_pattern = std::move(other.compiled_pattern);
        parameters = std::move(other.parameters);
        request_body_schema = std::move(other.request_body_schema);
        response_schema = std::move(other.response_schema);
        handler = other.handler;
        summary = std::move(other.summary);
        description = std::move(other.description);
        tags = std::move(other.tags);
        responses = std::move(other.responses);

        // Transfer ownership of PyObject*
        other.handler = nullptr;
    }
    return *this;
}

// ============================================================================
// RouteRegistry Implementation
// ============================================================================

int RouteRegistry::register_route(RouteMetadata metadata) {
    // Validate route
    if (metadata.method.empty()) {
        LOG_ERROR("RouteRegistry", "Cannot register route with empty method");
        return -1;
    }
    if (metadata.path_pattern.empty()) {
        LOG_ERROR("RouteRegistry", "Cannot register route with empty path pattern");
        return -1;
    }
    if (metadata.handler == nullptr) {
        LOG_ERROR("RouteRegistry", "Cannot register route with null handler");
        return -1;
    }

    // Increment reference count for the Python handler
    Py_INCREF(metadata.handler);

    // Add to method index
    size_t route_index = routes_.size();
    method_index_[metadata.method].push_back(route_index);

    // Store route
    routes_.push_back(std::move(metadata));

    LOG_DEBUG("RouteRegistry", "Registered route: %s %s",
              routes_[route_index].method.c_str(),
              routes_[route_index].path_pattern.c_str());

    return 0;
}

const RouteMetadata* RouteRegistry::match(
    const std::string& method,
    const std::string& path
) const noexcept {
    // Look up routes for this method
    auto it = method_index_.find(method);
    if (it == method_index_.end()) {
        return nullptr;
    }

    // Try to match path against each route for this method
    for (size_t route_idx : it->second) {
        const RouteMetadata& route = routes_[route_idx];
        if (route.compiled_pattern.matches(path)) {
            return &route;
        }
    }

    return nullptr;
}

void RouteRegistry::clear() {
    // Python handlers will be cleaned up by RouteMetadata destructors
    routes_.clear();
    method_index_.clear();
    LOG_DEBUG("RouteRegistry", "Cleared all routes");
}

} // namespace http
} // namespace fasterapi
