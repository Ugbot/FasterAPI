#pragma once

#include "../protocol/message.h"
#include "../protocol/session.h"
#include "../transports/transport.h"
#include <memory>
#include <future>
#include <unordered_map>
#include <mutex>

namespace fasterapi {
namespace mcp {

/**
 * MCP Client for calling remote MCP servers.
 *
 * Features:
 * - Connect to MCP servers via various transports
 * - Call tools
 * - Read resources
 * - Get prompts
 * - Async request handling with futures
 */
class MCPClient {
public:
    struct Config {
        std::string client_name = "FasterAPI MCP Client";
        std::string client_version = "0.1.0";
        uint32_t request_timeout_ms = 30000;  // 30 seconds
    };

    explicit MCPClient(const Config& config = Config());
    ~MCPClient();

    /**
     * Connect to MCP server using transport.
     *
     * @param transport Transport to use
     * @return 0 on success, negative on error
     */
    int connect(std::unique_ptr<Transport> transport);

    /**
     * Disconnect from server.
     */
    void disconnect();

    /**
     * Check if connected.
     */
    bool is_connected() const;

    /**
     * List available tools from server.
     *
     * @return Vector of tools
     */
    std::vector<Tool> list_tools();

    /**
     * Call a tool on the server.
     *
     * @param name Tool name
     * @param params Parameters as JSON string
     * @return Tool result
     */
    ToolResult call_tool(const std::string& name, const std::string& params);

    /**
     * Call a tool asynchronously.
     *
     * @param name Tool name
     * @param params Parameters as JSON string
     * @return Future that will contain the result
     */
    std::future<ToolResult> call_tool_async(const std::string& name, const std::string& params);

    /**
     * List available resources from server.
     *
     * @return Vector of resources
     */
    std::vector<Resource> list_resources();

    /**
     * Read a resource from the server.
     *
     * @param uri Resource URI
     * @return Resource content, or nullopt if not found
     */
    std::optional<ResourceContent> read_resource(const std::string& uri);

    /**
     * List available prompts from server.
     *
     * @return Vector of prompts
     */
    std::vector<Prompt> list_prompts();

    /**
     * Get a prompt from the server.
     *
     * @param name Prompt name
     * @param args Arguments
     * @return Prompt content, or nullopt if not found
     */
    std::optional<std::string> get_prompt(
        const std::string& name,
        const std::vector<std::string>& args = {}
    );

private:
    Config config_;
    std::unique_ptr<Transport> transport_;
    std::shared_ptr<Session> session_;
    std::atomic<bool> connected_{false};

    // Request tracking
    struct PendingRequest {
        std::promise<std::string> promise;
        std::chrono::steady_clock::time_point timeout;
    };

    std::unordered_map<std::string, PendingRequest> pending_requests_;
    std::mutex requests_mutex_;
    std::atomic<uint64_t> request_counter_{0};

    // Message handling
    void handle_message(const std::string& message_str);
    void handle_response(const JsonRpcResponse& resp);

    // Send request and wait for response
    std::string send_request_sync(const std::string& method, const std::string& params);

    // Send request and get future
    std::future<std::string> send_request_async(const std::string& method, const std::string& params);

    // Generate unique request ID
    std::string generate_request_id();
};

} // namespace mcp
} // namespace fasterapi
