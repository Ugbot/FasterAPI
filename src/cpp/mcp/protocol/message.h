#pragma once

#include <string>
#include <optional>
#include <variant>
#include <vector>
#include <unordered_map>
#include <memory>

namespace fasterapi {
namespace mcp {

/**
 * JSON-RPC 2.0 message types for Model Context Protocol.
 *
 * MCP is built on JSON-RPC 2.0 specification:
 * - Requests have method + params + id
 * - Responses have result/error + id
 * - Notifications have method + params (no id)
 */

// JSON value type (simplified)
using JsonValue = std::variant<
    std::nullptr_t,
    bool,
    int64_t,
    double,
    std::string,
    std::vector<std::shared_ptr<void>>,  // Array (recursive)
    std::unordered_map<std::string, std::shared_ptr<void>>  // Object (recursive)
>;

// JSON-RPC error codes
enum class ErrorCode : int {
    // JSON-RPC standard errors
    PARSE_ERROR = -32700,
    INVALID_REQUEST = -32600,
    METHOD_NOT_FOUND = -32601,
    INVALID_PARAMS = -32602,
    INTERNAL_ERROR = -32603,

    // MCP-specific errors
    SERVER_ERROR_START = -32000,
    SERVER_ERROR_END = -32099,

    // Custom error codes
    UNAUTHORIZED = -32001,
    FORBIDDEN = -32002,
    NOT_FOUND = -32003,
    TIMEOUT = -32004,
    RATE_LIMITED = -32005,
    INVALID_SCHEMA = -32006,
};

/**
 * JSON-RPC error object
 */
struct JsonRpcError {
    ErrorCode code;
    std::string message;
    std::optional<std::string> data;

    JsonRpcError(ErrorCode c, std::string msg, std::optional<std::string> d = std::nullopt)
        : code(c), message(std::move(msg)), data(std::move(d)) {}
};

/**
 * JSON-RPC request message
 */
struct JsonRpcRequest {
    std::string jsonrpc = "2.0";  // Always "2.0"
    std::string method;
    std::optional<std::string> params;  // JSON string
    std::optional<std::string> id;  // String or number (we use string)

    JsonRpcRequest() = default;
    JsonRpcRequest(std::string m, std::optional<std::string> p = std::nullopt, std::optional<std::string> i = std::nullopt)
        : method(std::move(m)), params(std::move(p)), id(std::move(i)) {}

    // Check if this is a notification (no response expected)
    bool is_notification() const { return !id.has_value(); }
};

/**
 * JSON-RPC response message
 */
struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    std::optional<std::string> result;  // JSON string (success)
    std::optional<JsonRpcError> error;  // Error object (failure)
    std::string id;  // Must match request id

    JsonRpcResponse() = default;

    // Success response
    static JsonRpcResponse success(std::string id, std::string result) {
        JsonRpcResponse resp;
        resp.id = std::move(id);
        resp.result = std::move(result);
        return resp;
    }

    // Error response
    static JsonRpcResponse error_response(std::string id, JsonRpcError err) {
        JsonRpcResponse resp;
        resp.id = std::move(id);
        resp.error = std::move(err);
        return resp;
    }

    bool is_error() const { return error.has_value(); }
};

/**
 * JSON-RPC notification message (no response expected)
 */
struct JsonRpcNotification {
    std::string jsonrpc = "2.0";
    std::string method;
    std::optional<std::string> params;  // JSON string

    JsonRpcNotification() = default;
    JsonRpcNotification(std::string m, std::optional<std::string> p = std::nullopt)
        : method(std::move(m)), params(std::move(p)) {}
};

/**
 * Union type for any JSON-RPC message
 */
using JsonRpcMessage = std::variant<JsonRpcRequest, JsonRpcResponse, JsonRpcNotification>;

/**
 * MCP protocol version
 */
struct ProtocolVersion {
    std::string version = "2024-11-05";  // Current MCP spec version
};

/**
 * MCP capabilities (client and server)
 */
struct Capabilities {
    // Server capabilities
    struct ServerCapabilities {
        bool tools = false;
        bool resources = false;
        bool prompts = false;
        bool logging = false;

        // Change notifications
        bool tools_list_changed = false;
        bool resources_list_changed = false;
        bool prompts_list_changed = false;
    } server;

    // Client capabilities
    struct ClientCapabilities {
        bool sampling = false;  // Can client perform LLM sampling?
        bool roots = false;     // Can client provide root URIs?
    } client;
};

/**
 * Implementation info
 */
struct Implementation {
    std::string name = "FasterAPI-MCP";
    std::string version = "0.1.0";
};

/**
 * Initialize request (sent by client)
 */
struct InitializeRequest {
    ProtocolVersion protocol_version;
    Capabilities capabilities;
    Implementation client_info;
};

/**
 * Initialize response (sent by server)
 */
struct InitializeResponse {
    ProtocolVersion protocol_version;
    Capabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions;  // Optional server instructions
};

/**
 * Tool definition
 */
struct Tool {
    std::string name;
    std::string description;
    std::optional<std::string> input_schema;  // JSON Schema
};

/**
 * Resource definition
 */
struct Resource {
    std::string uri;
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> mime_type;
};

/**
 * Prompt definition
 */
struct Prompt {
    std::string name;
    std::string description;
    std::optional<std::vector<std::string>> arguments;
};

/**
 * Tool call result
 */
struct ToolResult {
    bool is_error = false;
    std::string content;  // JSON string
    std::optional<std::string> error_message;
};

/**
 * Resource content
 */
struct ResourceContent {
    std::string uri;
    std::string mime_type;
    std::string content;  // Text or base64-encoded binary
};

/**
 * Message parser/serializer
 */
class MessageCodec {
public:
    // Parse JSON-RPC message from string
    static std::optional<JsonRpcMessage> parse(const std::string& json) noexcept;

    // Serialize JSON-RPC message to string
    static std::string serialize(const JsonRpcMessage& msg) noexcept;

    // Parse specific message types
    static std::optional<InitializeRequest> parse_initialize_request(const std::string& json) noexcept;
    static std::optional<InitializeResponse> parse_initialize_response(const std::string& json) noexcept;
    static std::optional<Tool> parse_tool(const std::string& json) noexcept;
    static std::optional<Resource> parse_resource(const std::string& json) noexcept;
    static std::optional<Prompt> parse_prompt(const std::string& json) noexcept;

    // Serialize specific message types
    static std::string serialize(const InitializeRequest& req) noexcept;
    static std::string serialize(const InitializeResponse& resp) noexcept;
    static std::string serialize(const Tool& tool) noexcept;
    static std::string serialize(const Resource& resource) noexcept;
    static std::string serialize(const Prompt& prompt) noexcept;

private:
    // Helper to create error response JSON
    static std::string create_error_response(const std::string& id, ErrorCode code, const std::string& message) noexcept;
};

} // namespace mcp
} // namespace fasterapi
