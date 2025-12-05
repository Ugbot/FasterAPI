#include "router.h"
#include "../core/logger.h"
#include <algorithm>
#include <iostream>

namespace fasterapi {
namespace http {

// ============================================================================
// RouteParams Implementation
// ============================================================================

void RouteParams::add(const std::string& key, const std::string& value) {
    params_.push_back({key, value});
}

std::string RouteParams::get(const std::string& key) const noexcept {
    for (const auto& param : params_) {
        if (param.key == key) {
            return param.value;
        }
    }
    return "";
}

const RouteParam& RouteParams::operator[](size_t index) const noexcept {
    static RouteParam empty;
    if (index < params_.size()) {
        return params_[index];
    }
    return empty;
}

// ============================================================================
// RouterNode Implementation
// ============================================================================

RouterNode* RouterNode::get_child(char c) const noexcept {
    // Try hash map first (O(1) lookup)
    auto it = child_map.find(c);
    if (it != child_map.end() && it->second < children.size()) {
        return children[it->second].get();
    }
    
    // Fallback to indices string (for backward compatibility)
    size_t idx = indices.find(c);
    if (idx != std::string::npos && idx < children.size()) {
        return children[idx].get();
    }
    
    return nullptr;
}

RouterNode* RouterNode::add_child(char c, NodeType node_type) {
    // Check if child already exists (via hash map)
    auto it = child_map.find(c);
    if (it != child_map.end() && it->second < children.size()) {
        return children[it->second].get();
    }
    
    // Create new child
    auto child = std::make_unique<RouterNode>(node_type);
    auto* child_ptr = child.get();
    
    // Add to children
    size_t idx = children.size();
    children.push_back(std::move(child));
    
    // Update indices and hash map
    indices += c;
    child_map[c] = idx;
    
    return child_ptr;
}

void RouterNode::increment_priority() noexcept {
    priority++;
}

// ============================================================================
// Router Implementation
// ============================================================================

Router::Router() : route_count_(0) {}

Router::~Router() = default;

Router::Router(Router&&) noexcept = default;
Router& Router::operator=(Router&&) noexcept = default;

int Router::add_route(
    const std::string& method,
    const std::string& path,
    RouteHandler handler
) noexcept {
    if (path.empty() || path[0] != '/') {
        std::cerr << "Router: path must start with '/': " << path << std::endl;
        return 1;
    }

    if (!handler) {
        std::cerr << "Router: handler cannot be null for path: " << path << std::endl;
        return 1;
    }

    // Get or create tree for method
    auto& tree = trees_[method];
    if (!tree) {
        tree = std::make_unique<RouterNode>(NodeType::STATIC);
        tree->path = "/";
    }

    // Special case: root path "/" - set handler directly on root node
    if (path == "/") {
        if (tree->handler) {
            std::cerr << "Router: duplicate route: " << path << std::endl;
            return 1;
        }
        tree->handler = std::move(handler);
        route_count_++;
        return 0;
    }

    // Insert route (start at pos=1 to skip root's '/')
    int result = insert_route(tree.get(), path, std::move(handler), 1);
    if (result == 0) {
        route_count_++;
    }

    return result;
}

RouteHandler Router::match(
    const std::string& method,
    const std::string& path,
    RouteParams& params
) const noexcept {
    // Clear params
    params.clear();

    // Find tree for method
    auto it = trees_.find(method);
    if (it == trees_.end()) {
        return nullptr;
    }

    // Special case: root path "/" - check root node's handler
    if (path == "/" && it->second->handler) {
        return it->second->handler;
    }

    // Match in tree (start at pos=1 to skip root's '/')
    return match_route(it->second.get(), path, params, 1);
}

size_t Router::route_count(const std::string& method) const noexcept {
    auto it = trees_.find(method);
    if (it == trees_.end()) {
        return 0;
    }
    
    // Count routes in tree
    size_t count = 0;
    std::function<void(const RouterNode*)> count_routes;
    count_routes = [&](const RouterNode* node) {
        if (node->handler) {
            count++;
        }
        for (const auto& child : node->children) {
            count_routes(child.get());
        }
    };
    
    count_routes(it->second.get());
    return count;
}

size_t Router::total_routes() const noexcept {
    return route_count_;
}

std::vector<Router::RouteInfo> Router::get_routes() const {
    std::vector<RouteInfo> routes;
    
    for (const auto& [method, tree] : trees_) {
        collect_routes(tree.get(), method, "", routes);
    }
    
    return routes;
}

// ============================================================================
// Private Implementation
// ============================================================================

int Router::insert_route(
    RouterNode* node,
    const std::string& path,
    RouteHandler handler,
    size_t pos
) noexcept {
    // Increment priority
    node->increment_priority();

    // If we've consumed the entire path
    if (pos >= path.length()) {
        if (node->handler) {
            return 1;  // Duplicate route
        }
        node->handler = std::move(handler);
        return 0;
    }
    
    // Find next segment boundary (next '/')
    size_t next_slash = path.find('/', pos + 1);
    if (next_slash == std::string::npos) {
        next_slash = path.length();
    }
    
    // Extract segment
    std::string segment = path.substr(pos, next_slash - pos);
    
    // Determine segment type
    NodeType seg_type = NodeType::STATIC;
    std::string param_name;
    
    if (is_wildcard(segment)) {
        seg_type = NodeType::WILDCARD;
        // Extract wildcard name: /*path -> path
        if (segment.length() > 2) {
            param_name = segment.substr(2);  // Skip "/*"
        }
    } else {
        param_name = parse_param_name(segment);
        if (!param_name.empty()) {
            seg_type = NodeType::PARAM;
        }
    }
    
    // Find or create child for this segment
    RouterNode* child = nullptr;
    bool need_new_child = false;  // Flag to track if we need to add remaining segment after split

    if (seg_type == NodeType::STATIC) {
        // Look for existing static child with matching prefix
        char first_char = segment.empty() ? '/' : segment[0];

        for (auto& c : node->children) {
            if (c->type == NodeType::STATIC && !c->path.empty() && c->path[0] == first_char) {
                // Found potential match
                size_t lcp = longest_common_prefix(c->path, segment);

                if (lcp == c->path.length() && lcp == segment.length()) {
                    // Exact match - continue with this child
                    child = c.get();
                    break;
                } else if (lcp == c->path.length() && lcp < segment.length()) {
                    // Child's path is prefix of segment - recursively descend into
                    // this child with the updated position (after matching the child's path)
                    return insert_route(c.get(), path, std::move(handler), pos + lcp);
                } else if (lcp > 0 && lcp < c->path.length()) {
                    // Need to split this node
                    auto split_node = std::make_unique<RouterNode>(NodeType::STATIC);
                    split_node->path = c->path.substr(0, lcp);
                    split_node->priority = c->priority;

                    // Old node becomes child of split node
                    c->path = c->path.substr(lcp);

                    // Update split node's child tracking
                    split_node->indices = std::string(1, c->path[0]);
                    split_node->child_map[c->path[0]] = 0;
                    split_node->children.push_back(std::move(c));

                    // Replace old child with split node
                    c = std::move(split_node);
                    child = c.get();

                    // Adjust segment for remaining path after common prefix
                    segment = segment.substr(lcp);
                    if (!segment.empty()) {
                        need_new_child = true;
                    }
                    break;
                }
            }
        }

        // Add new child for remaining segment if needed
        if (need_new_child && !segment.empty()) {
            auto new_child = std::make_unique<RouterNode>(NodeType::STATIC);
            new_child->path = segment;
            RouterNode* new_child_ptr = new_child.get();

            // Add to current child (the split node or prefix-matched node)
            child->indices += segment[0];
            child->child_map[segment[0]] = child->children.size();
            child->children.push_back(std::move(new_child));
            child = new_child_ptr;
        } else if (!child && !segment.empty()) {
            // Create new static child directly under current node
            auto new_child = std::make_unique<RouterNode>(NodeType::STATIC);
            new_child->path = segment;
            child = new_child.get();

            node->indices += segment[0];
            size_t child_idx = node->children.size();
            node->children.push_back(std::move(new_child));
            node->child_map[segment[0]] = child_idx;
        }
    } else {
        // Parameter or wildcard node
        // Look for existing param/wildcard child
        for (auto& c : node->children) {
            if (c->type == seg_type) {
                child = c.get();
                break;
            }
        }
        
        if (!child) {
            // Create new param/wildcard child
            auto new_child = std::make_unique<RouterNode>(seg_type);
            new_child->path = segment;
            new_child->param_name = param_name;
            child = new_child.get();
            
            // Add special marker for param/wildcard
            char marker = (seg_type == NodeType::PARAM) ? ':' : '*';
            node->indices += marker;
            node->children.push_back(std::move(new_child));
        }
    }
    
    // Continue with remaining path
    return insert_route(child, path, std::move(handler), next_slash);
}

RouteHandler Router::match_route(
    const RouterNode* node,
    const std::string& path,
    RouteParams& params,
    size_t pos
) const noexcept {
    if (!node) {
        return nullptr;
    }

    // Check if we've matched the entire path
    if (pos >= path.length()) {
        return node->handler;
    }
    
    // Try children in priority order: static > param > wildcard
    
    // 1. Try static children first (highest priority)
    // Optimize: Try hash map lookup if we have the first character
    if (pos < path.length()) {
        char first_char = path[pos];
        auto it = node->child_map.find(first_char);
        if (it != node->child_map.end() && it->second < node->children.size()) {
            const auto& child = node->children[it->second];
            if (child->type == NodeType::STATIC) {
                // Check if path matches child's path
                if (pos + child->path.length() <= path.length()) {
                    bool matches = true;
                    for (size_t i = 0; i < child->path.length(); ++i) {
                        if (path[pos + i] != child->path[i]) {
                            matches = false;
                            break;
                        }
                    }

                    if (matches) {
                        auto handler = match_route(child.get(), path, params, pos + child->path.length());
                        if (handler) {
                            return handler;
                        }
                    }
                }
            }
        }
    }
    
    // Fallback: Try all static children (for nodes with multiple children with same first char)
    for (const auto& child : node->children) {
        if (child->type != NodeType::STATIC) {
            continue;
        }
        
        // Skip if already tried via hash map
        if (pos < path.length() && !child->path.empty() && 
            node->child_map.find(child->path[0]) != node->child_map.end()) {
            continue;
        }
        
        // Check if path matches child's path
        if (pos + child->path.length() <= path.length()) {
            bool matches = true;
            for (size_t i = 0; i < child->path.length(); ++i) {
                if (path[pos + i] != child->path[i]) {
                    matches = false;
                    break;
                }
            }
            
            if (matches) {
                auto handler = match_route(child.get(), path, params, pos + child->path.length());
                if (handler) {
                    return handler;
                }
            }
        }
    }
    
    // 2. Try parameter children
    for (const auto& child : node->children) {
        if (child->type != NodeType::PARAM) {
            continue;
        }

        // Extract parameter value until next '/' or end
        size_t next_slash = path.find('/', pos + 1);
        if (next_slash == std::string::npos) {
            next_slash = path.length();
        }

        if (next_slash > pos + 1) {  // Must have at least one character
            std::string value = path.substr(pos + 1, next_slash - pos - 1);

            // Save param value
            size_t param_count_before = params.size();
            params.add(child->param_name, value);

            // Try to match rest of path
            auto handler = match_route(child.get(), path, params, next_slash);
            if (handler) {
                return handler;
            }

            // Backtrack: remove param if match failed
            while (params.size() > param_count_before) {
                params.clear();  // Simple clear for now
            }
        }
    }
    
    // 3. Try wildcard children (lowest priority)
    for (const auto& child : node->children) {
        if (child->type != NodeType::WILDCARD) {
            continue;
        }
        
        // Wildcard matches rest of path from current position
        // If path is "/files/a/b/c.txt" and pos is at "/files/",
        // we want to extract "a/b/c.txt"
        std::string value;
        if (pos < path.length()) {
            // Skip the leading slash if present
            size_t value_start = (pos < path.length() && path[pos] == '/') ? pos + 1 : pos;
            value = path.substr(value_start);
        }
        params.add(child->param_name, value);
        return child->handler;
    }
    
    return nullptr;
}

size_t Router::longest_common_prefix(
    const std::string& a,
    const std::string& b,
    size_t start
) noexcept {
    size_t len = std::min(a.length() - start, b.length() - start);
    size_t i = 0;
    
    while (i < len && a[start + i] == b[start + i]) {
        i++;
    }
    
    return i;
}

std::string Router::parse_param_name(const std::string& segment) noexcept {
    // Check if segment is a parameter: /{name}
    if (segment.length() >= 3 && segment[0] == '/' && segment[1] == '{') {
        size_t close = segment.find('}', 2);
        if (close != std::string::npos) {
            return segment.substr(2, close - 2);
        }
    }
    return "";
}

bool Router::is_wildcard(const std::string& segment) noexcept {
    // Check if segment starts with "/*"
    return segment.length() >= 2 && segment[0] == '/' && segment[1] == '*';
}

void Router::collect_routes(
    const RouterNode* node,
    const std::string& method,
    const std::string& prefix,
    std::vector<RouteInfo>& routes
) const {
    if (!node) {
        return;
    }
    
    // Build current path based on node type
    std::string current_path;
    
    if (node->type == NodeType::PARAM) {
        // Reconstruct parameter syntax: /{paramName}
        current_path = prefix + "/{" + node->param_name + "}";
    } else if (node->type == NodeType::WILDCARD) {
        // Reconstruct wildcard syntax: /*paramName
        current_path = prefix + "/*" + node->param_name;
    } else {
        // Static node - avoid double slashes
        if (prefix.empty() || prefix == "/") {
            current_path = node->path;
        } else if (node->path == "/") {
            current_path = prefix;
        } else {
            current_path = prefix + node->path;
        }
    }
    
    if (node->handler) {
        routes.push_back({method, current_path, node->priority});
    }
    
    for (const auto& child : node->children) {
        collect_routes(child.get(), method, current_path, routes);
    }
}

} // namespace http
} // namespace fasterapi

