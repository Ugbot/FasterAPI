#pragma once

#include "../protocol/message.h"
#include "../protocol/session.h"
#include "../transports/transport.h"
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace fasterapi {
namespace mcp {

/**
 * Tool handler function type.
 * Takes parameters as JSON string, returns result as JSON string.
 * Throws exception on error.
 */
using ToolHandler = std::function<std::string(const std::string& params)>;

/**
 * Resource content provider function type.
 * Returns resource content for given URI.
 */
using ResourceProvider = std::function<ResourceContent(const std::string& uri)>;

/**
 * Prompt generator function type.
 * Takes arguments, returns prompt content.
 */
using PromptGenerator = std::function<std::string(const std::vector<std::string>& args)>;

/**
 * Tool registry manages available tools.
 */
class ToolRegistry {
public:
    /**
     * Register a tool.
     *
     * @param tool Tool definition
     * @param handler Handler function
     * @return true if registered, false if already exists
     */
    bool register_tool(const Tool& tool, ToolHandler handler);

    /**
     * Unregister a tool.
     *
     * @param name Tool name
     * @return true if unregistered, false if not found
     */
    bool unregister_tool(const std::string& name);

    /**
     * Get all registered tools.
     */
    std::vector<Tool> list_tools() const;

    /**
     * Get tool by name.
     */
    std::optional<Tool> get_tool(const std::string& name) const;

    /**
     * Call a tool.
     *
     * @param name Tool name
     * @param params Parameters as JSON string
     * @return Tool result
     */
    ToolResult call_tool(const std::string& name, const std::string& params);

    /**
     * Check if tool exists.
     */
    bool has_tool(const std::string& name) const;

private:
    struct ToolEntry {
        Tool definition;
        ToolHandler handler;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ToolEntry> tools_;
};

/**
 * Resource registry manages available resources.
 */
class ResourceRegistry {
public:
    /**
     * Register a resource.
     *
     * @param resource Resource definition
     * @param provider Provider function
     * @return true if registered, false if already exists
     */
    bool register_resource(const Resource& resource, ResourceProvider provider);

    /**
     * Unregister a resource.
     *
     * @param uri Resource URI
     * @return true if unregistered, false if not found
     */
    bool unregister_resource(const std::string& uri);

    /**
     * Get all registered resources.
     */
    std::vector<Resource> list_resources() const;

    /**
     * Get resource by URI.
     */
    std::optional<Resource> get_resource(const std::string& uri) const;

    /**
     * Read resource content.
     *
     * @param uri Resource URI
     * @return Resource content, or nullopt if not found
     */
    std::optional<ResourceContent> read_resource(const std::string& uri);

    /**
     * Check if resource exists.
     */
    bool has_resource(const std::string& uri) const;

private:
    struct ResourceEntry {
        Resource definition;
        ResourceProvider provider;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ResourceEntry> resources_;
};

/**
 * Prompt registry manages available prompts.
 */
class PromptRegistry {
public:
    /**
     * Register a prompt.
     *
     * @param prompt Prompt definition
     * @param generator Generator function
     * @return true if registered, false if already exists
     */
    bool register_prompt(const Prompt& prompt, PromptGenerator generator);

    /**
     * Unregister a prompt.
     *
     * @param name Prompt name
     * @return true if unregistered, false if not found
     */
    bool unregister_prompt(const std::string& name);

    /**
     * Get all registered prompts.
     */
    std::vector<Prompt> list_prompts() const;

    /**
     * Get prompt by name.
     */
    std::optional<Prompt> get_prompt(const std::string& name) const;

    /**
     * Get prompt content.
     *
     * @param name Prompt name
     * @param args Arguments
     * @return Prompt content, or nullopt if not found
     */
    std::optional<std::string> get_prompt_content(
        const std::string& name,
        const std::vector<std::string>& args
    );

    /**
     * Check if prompt exists.
     */
    bool has_prompt(const std::string& name) const;

private:
    struct PromptEntry {
        Prompt definition;
        PromptGenerator generator;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, PromptEntry> prompts_;
};

/**
 * MCP Server implementation.
 *
 * Handles:
 * - Protocol negotiation
 * - Tool/resource/prompt management
 * - Request routing
 * - Session management
 */
class MCPServer {
public:
    /**
     * Server configuration.
     */
    struct Config {
        std::string name = "FasterAPI MCP Server";
        std::string version = "0.1.0";
        std::string instructions;  // Optional server instructions

        // Capabilities
        bool enable_tools = true;
        bool enable_resources = true;
        bool enable_prompts = true;
        bool enable_logging = false;

        // Change notifications
        bool notify_tools_changed = true;
        bool notify_resources_changed = true;
        bool notify_prompts_changed = true;
    };

    explicit MCPServer(const Config& config = Config());
    ~MCPServer();

    /**
     * Start server with transport.
     *
     * @param transport Transport to use
     * @return 0 on success, negative on error
     */
    int start(std::unique_ptr<Transport> transport);

    /**
     * Stop server.
     */
    void stop();

    /**
     * Check if server is running.
     */
    bool is_running() const { return running_; }

    /**
     * Get tool registry.
     */
    ToolRegistry& tools() { return tool_registry_; }

    /**
     * Get resource registry.
     */
    ResourceRegistry& resources() { return resource_registry_; }

    /**
     * Get prompt registry.
     */
    PromptRegistry& prompts() { return prompt_registry_; }

    /**
     * Get session manager.
     */
    SessionManager& sessions() { return session_manager_; }

private:
    Config config_;
    std::atomic<bool> running_{false};

    // Registries
    ToolRegistry tool_registry_;
    ResourceRegistry resource_registry_;
    PromptRegistry prompt_registry_;

    // Session management
    SessionManager session_manager_;
    std::shared_ptr<Session> current_session_;

    // Transport
    std::unique_ptr<Transport> transport_;

    // Message handling
    void handle_message(const std::string& message_str);
    void handle_request(const JsonRpcRequest& req);
    void handle_notification(const JsonRpcNotification& notif);

    // Protocol methods
    void handle_initialize(const JsonRpcRequest& req);
    void handle_initialized(const JsonRpcNotification& notif);
    void handle_tools_list(const JsonRpcRequest& req);
    void handle_tools_call(const JsonRpcRequest& req);
    void handle_resources_list(const JsonRpcRequest& req);
    void handle_resources_read(const JsonRpcRequest& req);
    void handle_prompts_list(const JsonRpcRequest& req);
    void handle_prompts_get(const JsonRpcRequest& req);

    // Send response/notification
    void send_response(const JsonRpcResponse& resp);
    void send_notification(const JsonRpcNotification& notif);
    void send_error(const std::string& id, ErrorCode code, const std::string& message);
};

} // namespace mcp
} // namespace fasterapi
