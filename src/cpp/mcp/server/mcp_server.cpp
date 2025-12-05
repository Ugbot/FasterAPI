#include "mcp_server.h"
#include <iostream>
#include <sstream>

namespace fasterapi {
namespace mcp {

// ToolRegistry implementation

bool ToolRegistry::register_tool(const Tool& tool, ToolHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = tools_.emplace(tool.name, ToolEntry{tool, handler});
    return inserted;
}

bool ToolRegistry::unregister_tool(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.erase(name) > 0;
}

std::vector<Tool> ToolRegistry::list_tools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Tool> result;
    result.reserve(tools_.size());
    for (const auto& [name, entry] : tools_) {
        result.push_back(entry.definition);
    }
    return result;
}

std::optional<Tool> ToolRegistry::get_tool(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    if (it != tools_.end()) {
        return it->second.definition;
    }
    return std::nullopt;
}

ToolResult ToolRegistry::call_tool(const std::string& name, const std::string& params) {
    ToolHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            return ToolResult{true, "", "Tool not found: " + name};
        }
        handler = it->second.handler;
    }

    try {
        std::string result = handler(params);
        return ToolResult{false, result, std::nullopt};
    } catch (const std::exception& e) {
        return ToolResult{true, "", e.what()};
    } catch (...) {
        return ToolResult{true, "", "Unknown error"};
    }
}

bool ToolRegistry::has_tool(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.find(name) != tools_.end();
}

// ResourceRegistry implementation

bool ResourceRegistry::register_resource(const Resource& resource, ResourceProvider provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = resources_.emplace(resource.uri, ResourceEntry{resource, provider});
    return inserted;
}

bool ResourceRegistry::unregister_resource(const std::string& uri) {
    std::lock_guard<std::mutex> lock(mutex_);
    return resources_.erase(uri) > 0;
}

std::vector<Resource> ResourceRegistry::list_resources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Resource> result;
    result.reserve(resources_.size());
    for (const auto& [uri, entry] : resources_) {
        result.push_back(entry.definition);
    }
    return result;
}

std::optional<Resource> ResourceRegistry::get_resource(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resources_.find(uri);
    if (it != resources_.end()) {
        return it->second.definition;
    }
    return std::nullopt;
}

std::optional<ResourceContent> ResourceRegistry::read_resource(const std::string& uri) {
    ResourceProvider provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = resources_.find(uri);
        if (it == resources_.end()) {
            return std::nullopt;
        }
        provider = it->second.provider;
    }

    try {
        return provider(uri);
    } catch (...) {
        return std::nullopt;
    }
}

bool ResourceRegistry::has_resource(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resources_.find(uri) != resources_.end();
}

// PromptRegistry implementation

bool PromptRegistry::register_prompt(const Prompt& prompt, PromptGenerator generator) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = prompts_.emplace(prompt.name, PromptEntry{prompt, generator});
    return inserted;
}

bool PromptRegistry::unregister_prompt(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return prompts_.erase(name) > 0;
}

std::vector<Prompt> PromptRegistry::list_prompts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Prompt> result;
    result.reserve(prompts_.size());
    for (const auto& [name, entry] : prompts_) {
        result.push_back(entry.definition);
    }
    return result;
}

std::optional<Prompt> PromptRegistry::get_prompt(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = prompts_.find(name);
    if (it != prompts_.end()) {
        return it->second.definition;
    }
    return std::nullopt;
}

std::optional<std::string> PromptRegistry::get_prompt_content(
    const std::string& name,
    const std::vector<std::string>& args
) {
    PromptGenerator generator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = prompts_.find(name);
        if (it == prompts_.end()) {
            return std::nullopt;
        }
        generator = it->second.generator;
    }

    try {
        return generator(args);
    } catch (...) {
        return std::nullopt;
    }
}

bool PromptRegistry::has_prompt(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return prompts_.find(name) != prompts_.end();
}

// MCPServer implementation

MCPServer::MCPServer(const Config& config)
    : config_(config)
{
}

MCPServer::~MCPServer() {
    stop();
}

int MCPServer::start(std::unique_ptr<Transport> transport) {
    if (running_) {
        return -1;  // Already running
    }

    transport_ = std::move(transport);

    // Set message callback
    transport_->set_message_callback([this](const std::string& message) {
        handle_message(message);
    });

    // Set error callback
    transport_->set_error_callback([](const std::string& error) {
        std::cerr << "Transport error: " << error << std::endl;
    });

    // Connect transport
    int result = transport_->connect();
    if (result != 0) {
        return result;
    }

    running_ = true;
    return 0;
}

void MCPServer::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (transport_) {
        transport_->disconnect();
    }

    session_manager_.close_all_sessions();
}

void MCPServer::handle_message(const std::string& message_str) {
    // Parse JSON-RPC message
    auto message_opt = MessageCodec::parse(message_str);
    if (!message_opt.has_value()) {
        send_error("", ErrorCode::PARSE_ERROR, "Invalid JSON-RPC message");
        return;
    }

    const auto& message = message_opt.value();

    // Route based on message type
    if (std::holds_alternative<JsonRpcRequest>(message)) {
        handle_request(std::get<JsonRpcRequest>(message));
    } else if (std::holds_alternative<JsonRpcNotification>(message)) {
        handle_notification(std::get<JsonRpcNotification>(message));
    }
    // Responses are ignored (we're the server)
}

void MCPServer::handle_request(const JsonRpcRequest& req) {
    // Route to appropriate handler
    if (req.method == "initialize") {
        handle_initialize(req);
    } else if (req.method == "tools/list") {
        handle_tools_list(req);
    } else if (req.method == "tools/call") {
        handle_tools_call(req);
    } else if (req.method == "resources/list") {
        handle_resources_list(req);
    } else if (req.method == "resources/read") {
        handle_resources_read(req);
    } else if (req.method == "prompts/list") {
        handle_prompts_list(req);
    } else if (req.method == "prompts/get") {
        handle_prompts_get(req);
    } else {
        send_error(req.id.value_or(""), ErrorCode::METHOD_NOT_FOUND, "Method not found: " + req.method);
    }
}

void MCPServer::handle_notification(const JsonRpcNotification& notif) {
    if (notif.method == "notifications/initialized") {
        handle_initialized(notif);
    }
    // Other notifications are logged but not acted upon
}

void MCPServer::handle_initialize(const JsonRpcRequest& req) {
    if (!req.params.has_value()) {
        send_error(req.id.value_or(""), ErrorCode::INVALID_PARAMS, "Missing params");
        return;
    }

    // Parse initialize request
    auto init_req_opt = MessageCodec::parse_initialize_request(req.params.value());
    if (!init_req_opt.has_value()) {
        send_error(req.id.value_or(""), ErrorCode::INVALID_PARAMS, "Invalid initialize params");
        return;
    }

    // Create session
    auto session_id = session_manager_.create_session(true);  // Server session
    current_session_ = session_manager_.get_session(session_id);

    // Build server capabilities
    Capabilities server_caps;
    server_caps.server.tools = config_.enable_tools;
    server_caps.server.resources = config_.enable_resources;
    server_caps.server.prompts = config_.enable_prompts;
    server_caps.server.logging = config_.enable_logging;
    server_caps.server.tools_list_changed = config_.notify_tools_changed;
    server_caps.server.resources_list_changed = config_.notify_resources_changed;
    server_caps.server.prompts_list_changed = config_.notify_prompts_changed;

    Implementation server_info;
    server_info.name = config_.name;
    server_info.version = config_.version;

    // Handle initialize
    auto resp = current_session_->handle_initialize_request(
        init_req_opt.value(),
        server_info,
        server_caps
    );

    send_response(resp);
}

void MCPServer::handle_initialized(const JsonRpcNotification& notif) {
    if (current_session_) {
        current_session_->handle_initialized_notification(notif);
    }
}

void MCPServer::handle_tools_list(const JsonRpcRequest& req) {
    auto tools = tool_registry_.list_tools();

    // Build JSON array
    std::ostringstream oss;
    oss << "{\"tools\":[";
    bool first = true;
    for (const auto& tool : tools) {
        if (!first) oss << ",";
        oss << MessageCodec::serialize(tool);
        first = false;
    }
    oss << "]}";

    send_response(JsonRpcResponse::success(req.id.value_or(""), oss.str()));
}

void MCPServer::handle_tools_call(const JsonRpcRequest& req) {
    // Extract tool name and params from request params
    // Format: {"name": "tool_name", "arguments": {...}}
    if (!req.params.has_value()) {
        send_error(req.id.value_or(""), ErrorCode::INVALID_PARAMS, "Missing params");
        return;
    }

    // Simple parsing (TODO: improve)
    std::string params_str = req.params.value();
    auto name_start = params_str.find("\"name\"");
    if (name_start == std::string::npos) {
        send_error(req.id.value_or(""), ErrorCode::INVALID_PARAMS, "Missing tool name");
        return;
    }

    // Extract tool name
    auto name_quote1 = params_str.find("\"", name_start + 7);
    auto name_quote2 = params_str.find("\"", name_quote1 + 1);
    std::string tool_name = params_str.substr(name_quote1 + 1, name_quote2 - name_quote1 - 1);

    // Extract arguments
    auto args_start = params_str.find("\"arguments\"");
    std::string tool_params = "{}";
    if (args_start != std::string::npos) {
        auto args_colon = params_str.find(":", args_start);
        auto args_open = params_str.find("{", args_colon);
        int depth = 1;
        size_t pos = args_open + 1;
        while (pos < params_str.length() && depth > 0) {
            if (params_str[pos] == '{') depth++;
            else if (params_str[pos] == '}') depth--;
            pos++;
        }
        tool_params = params_str.substr(args_open, pos - args_open);
    }

    // Call tool
    auto result = tool_registry_.call_tool(tool_name, tool_params);

    if (result.is_error) {
        send_error(req.id.value_or(""), ErrorCode::INTERNAL_ERROR, result.error_message.value_or("Tool execution failed"));
    } else {
        std::string response = "{\"content\":[{\"type\":\"text\",\"text\":" + result.content + "}]}";
        send_response(JsonRpcResponse::success(req.id.value_or(""), response));
    }
}

void MCPServer::handle_resources_list(const JsonRpcRequest& req) {
    auto resources = resource_registry_.list_resources();

    std::ostringstream oss;
    oss << "{\"resources\":[";
    bool first = true;
    for (const auto& resource : resources) {
        if (!first) oss << ",";
        oss << MessageCodec::serialize(resource);
        first = false;
    }
    oss << "]}";

    send_response(JsonRpcResponse::success(req.id.value_or(""), oss.str()));
}

void MCPServer::handle_resources_read(const JsonRpcRequest& req) {
    // Extract URI from params
    if (!req.params.has_value()) {
        send_error(req.id.value_or(""), ErrorCode::INVALID_PARAMS, "Missing params");
        return;
    }

    // Simple URI extraction (TODO: improve)
    std::string params_str = req.params.value();
    auto uri_start = params_str.find("\"uri\"");
    if (uri_start == std::string::npos) {
        send_error(req.id.value_or(""), ErrorCode::INVALID_PARAMS, "Missing URI");
        return;
    }

    auto uri_quote1 = params_str.find("\"", uri_start + 5);
    auto uri_quote2 = params_str.find("\"", uri_quote1 + 1);
    std::string uri = params_str.substr(uri_quote1 + 1, uri_quote2 - uri_quote1 - 1);

    // Read resource
    auto content_opt = resource_registry_.read_resource(uri);
    if (!content_opt.has_value()) {
        send_error(req.id.value_or(""), ErrorCode::NOT_FOUND, "Resource not found");
        return;
    }

    const auto& content = content_opt.value();
    std::ostringstream oss;
    oss << "{\"contents\":[{\"uri\":\"" << content.uri << "\","
        << "\"mimeType\":\"" << content.mime_type << "\","
        << "\"text\":\"" << content.content << "\"}]}";

    send_response(JsonRpcResponse::success(req.id.value_or(""), oss.str()));
}

void MCPServer::handle_prompts_list(const JsonRpcRequest& req) {
    auto prompts = prompt_registry_.list_prompts();

    std::ostringstream oss;
    oss << "{\"prompts\":[";
    bool first = true;
    for (const auto& prompt : prompts) {
        if (!first) oss << ",";
        oss << MessageCodec::serialize(prompt);
        first = false;
    }
    oss << "]}";

    send_response(JsonRpcResponse::success(req.id.value_or(""), oss.str()));
}

void MCPServer::handle_prompts_get(const JsonRpcRequest& req) {
    // Extract prompt name and args
    if (!req.params.has_value()) {
        send_error(req.id.value_or(""), ErrorCode::INVALID_PARAMS, "Missing params");
        return;
    }

    // TODO: Proper JSON parsing
    std::string params_str = req.params.value();

    // Extract name
    auto name_start = params_str.find("\"name\"");
    if (name_start == std::string::npos) {
        send_error(req.id.value_or(""), ErrorCode::INVALID_PARAMS, "Missing prompt name");
        return;
    }

    auto name_quote1 = params_str.find("\"", name_start + 6);
    auto name_quote2 = params_str.find("\"", name_quote1 + 1);
    std::string prompt_name = params_str.substr(name_quote1 + 1, name_quote2 - name_quote1 - 1);

    // Get prompt content
    std::vector<std::string> args;  // TODO: Parse arguments
    auto content_opt = prompt_registry_.get_prompt_content(prompt_name, args);

    if (!content_opt.has_value()) {
        send_error(req.id.value_or(""), ErrorCode::NOT_FOUND, "Prompt not found");
        return;
    }

    std::string response = "{\"description\":\"Generated prompt\",\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"" + content_opt.value() + "\"}}]}";
    send_response(JsonRpcResponse::success(req.id.value_or(""), response));
}

void MCPServer::send_response(const JsonRpcResponse& resp) {
    std::string json = MessageCodec::serialize(JsonRpcMessage(resp));
    transport_->send(json);
}

void MCPServer::send_notification(const JsonRpcNotification& notif) {
    std::string json = MessageCodec::serialize(JsonRpcMessage(notif));
    transport_->send(json);
}

void MCPServer::send_error(const std::string& id, ErrorCode code, const std::string& message) {
    JsonRpcResponse resp = JsonRpcResponse::error_response(
        id,
        JsonRpcError(code, message)
    );
    send_response(resp);
}

} // namespace mcp
} // namespace fasterapi
