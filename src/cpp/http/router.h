#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <cstdint>

// Forward declarations (before namespace to avoid conflicts)
struct HttpRequest;
struct HttpResponse;

namespace fasterapi {
namespace http {

/**
 * High-performance radix tree router with path parameter extraction.
 * 
 * Features:
 * - Path parameters: /user/{id} matches /user/123
 * - Wildcard routes: /files/*path matches /files/a/b/c
 * - Priority matching: static > param > wildcard
 * - O(k) lookup where k = path length
 * - Zero allocations during match (pre-allocated param storage)
 * - Thread-safe for concurrent reads
 * 
 * Design based on:
 * - httprouter (Go) - https://github.com/julienschmidt/httprouter
 * - Gin router (Go) - https://github.com/gin-gonic/gin
 * - chi router (Go) - https://github.com/go-chi/chi
 */

/**
 * Route parameter extracted from path.
 */
struct RouteParam {
    std::string key;    // Parameter name (without braces)
    std::string value;  // Extracted value from path
};

/**
 * Collection of route parameters.
 */
class RouteParams {
public:
    RouteParams() = default;
    
    /**
     * Add a parameter.
     */
    void add(const std::string& key, const std::string& value);
    
    /**
     * Get parameter value by key.
     * 
     * @param key Parameter name
     * @return Parameter value, or empty string if not found
     */
    std::string get(const std::string& key) const noexcept;
    
    /**
     * Get parameter by index.
     */
    const RouteParam& operator[](size_t index) const noexcept;
    
    /**
     * Get number of parameters.
     */
    size_t size() const noexcept { return params_.size(); }
    
    /**
     * Clear all parameters.
     */
    void clear() noexcept { params_.clear(); }
    
    /**
     * Check if empty.
     */
    bool empty() const noexcept { return params_.empty(); }
    
private:
    std::vector<RouteParam> params_;
};

/**
 * Route handler function type.
 */
using RouteHandler = std::function<void(HttpRequest*, HttpResponse*, const RouteParams&)>;

/**
 * Node type in radix tree.
 */
enum class NodeType : uint8_t {
    STATIC,    // Static path segment (e.g., "/user")
    PARAM,     // Parameter segment (e.g., "/{id}")
    WILDCARD   // Wildcard segment (e.g., "/*path")
};

/**
 * Node in radix tree.
 */
struct RouterNode {
    // Node configuration
    std::string path;           // Path segment for this node
    std::string param_name;     // Parameter name (for PARAM and WILDCARD nodes)
    NodeType type;              // Type of this node
    
    // Handler
    RouteHandler handler;       // Handler function (nullptr if intermediate node)
    
    // Children
    std::vector<std::unique_ptr<RouterNode>> children;
    
    // Indices for faster child lookup
    std::string indices;        // First char of each child's path
    
    // Hash map for O(1) static child lookup (optimization for many children)
    std::unordered_map<char, size_t> child_map;  // first_char -> index in children
    
    // Priority for ordering (higher = checked first)
    uint32_t priority;
    
    RouterNode(NodeType t = NodeType::STATIC)
        : type(t), priority(0) {}
    
    /**
     * Get child node by index character.
     */
    RouterNode* get_child(char c) const noexcept;
    
    /**
     * Add or get child with given first character.
     */
    RouterNode* add_child(char c, NodeType type);
    
    /**
     * Increment priority (and propagate to parent).
     */
    void increment_priority() noexcept;
};

/**
 * High-performance HTTP router using radix tree.
 */
class Router {
public:
    Router();
    ~Router();
    
    // Non-copyable, movable
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) noexcept;
    Router& operator=(Router&&) noexcept;
    
    /**
     * Add a route to the router.
     * 
     * Path patterns:
     * - Static: "/users" - exact match
     * - Parameter: "/users/{id}" - matches /users/123, extracts id=123
     * - Wildcard: "/files/*path" - matches /files/a/b/c, extracts path=a/b/c
     * 
     * @param method HTTP method (GET, POST, etc.)
     * @param path Route path pattern
     * @param handler Handler function
     * @return 0 on success, error code otherwise
     */
    int add_route(
        const std::string& method,
        const std::string& path,
        RouteHandler handler
    ) noexcept;
    
    /**
     * Match a request path and extract parameters.
     * 
     * @param method HTTP method
     * @param path Request path
     * @param params Output parameters (cleared before use)
     * @return Handler function, or nullptr if no match
     */
    RouteHandler match(
        const std::string& method,
        const std::string& path,
        RouteParams& params
    ) const noexcept;
    
    /**
     * Get route count for a method.
     */
    size_t route_count(const std::string& method) const noexcept;
    
    /**
     * Get total route count.
     */
    size_t total_routes() const noexcept;
    
    /**
     * Get all registered routes (for introspection/debugging).
     */
    struct RouteInfo {
        std::string method;
        std::string path;
        uint32_t priority;
    };
    std::vector<RouteInfo> get_routes() const;
    
private:
    // Per-method trees
    std::unordered_map<std::string, std::unique_ptr<RouterNode>> trees_;
    
    // Route count
    size_t route_count_;
    
    /**
     * Insert a route into the tree.
     */
    int insert_route(
        RouterNode* root,
        const std::string& path,
        RouteHandler handler,
        size_t pos = 0
    ) noexcept;
    
    /**
     * Match a path in the tree.
     */
    RouteHandler match_route(
        const RouterNode* node,
        const std::string& path,
        RouteParams& params,
        size_t pos = 0
    ) const noexcept;
    
    /**
     * Find longest common prefix.
     */
    static size_t longest_common_prefix(
        const std::string& a,
        const std::string& b,
        size_t start = 0
    ) noexcept;
    
    /**
     * Parse parameter name from path segment.
     * Returns empty string if not a parameter.
     */
    static std::string parse_param_name(const std::string& segment) noexcept;
    
    /**
     * Check if segment is a wildcard.
     */
    static bool is_wildcard(const std::string& segment) noexcept;
    
    /**
     * Collect all routes from tree (for introspection).
     */
    void collect_routes(
        const RouterNode* node,
        const std::string& method,
        const std::string& prefix,
        std::vector<RouteInfo>& routes
    ) const;
};

} // namespace http
} // namespace fasterapi
