#include "mcp_client.h"
#include "../transports/stdio_transport.h"
#include <sstream>
#include <chrono>

namespace fasterapi {
namespace mcp {

MCPClient::MCPClient(const Config& config)
    : config_(config)
{
}

MCPClient::~MCPClient() {
    disconnect();
}

int MCPClient::connect(std::unique_ptr<Transport> transport) {
    if (connected_) {
        return -1;  // Already connected
    }

    transport_ = std::move(transport);

    // Set message callback to handle responses
    transport_->set_message_callback([this](const std::string& message) {
        handle_message(message);
    });

    // Connect transport
    int result = transport_->connect();
    if (result != 0) {
        return result;
    }

    // Create session
    session_ = std::make_shared<Session>(false);  // Client session

    // Send initialize request
    Capabilities client_caps;
    client_caps.client.sampling = false;
    client_caps.client.roots = false;

    Implementation client_info;
    client_info.name = config_.client_name;
    client_info.version = config_.client_version;

    InitializeRequest init_req;
    init_req.protocol_version = ProtocolVersion();
    init_req.capabilities = client_caps;
    init_req.client_info = client_info;

    // Build initialize request JSON-RPC message
    JsonRpcRequest req("initialize", MessageCodec::serialize(init_req), generate_request_id());

    std::string req_json = MessageCodec::serialize(JsonRpcMessage(req));
    int send_result = transport_->send(req_json);
    if (send_result != 0) {
        return send_result;
    }

    // Wait for initialize response (with timeout)
    auto response = transport_->receive(config_.request_timeout_ms);
    if (!response.has_value()) {
        return -2;  // Timeout
    }

    // Parse initialize response
    auto message_opt = MessageCodec::parse(response.value());
    if (!message_opt.has_value()) {
        return -3;  // Invalid response
    }

    // Verify it's a response
    if (!std::holds_alternative<JsonRpcResponse>(*message_opt)) {
        return -4;  // Not a response
    }

    const auto& resp = std::get<JsonRpcResponse>(*message_opt);
    if (resp.is_error()) {
        return -5;  // Initialize error
    }

    // Send initialized notification
    JsonRpcNotification initialized_notif("notifications/initialized");
    std::string notif_json = MessageCodec::serialize(JsonRpcMessage(initialized_notif));
    transport_->send(notif_json);

    connected_ = true;
    return 0;
}

void MCPClient::disconnect() {
    if (!connected_) {
        return;
    }

    connected_ = false;

    if (transport_) {
        transport_->disconnect();
    }

    session_.reset();
}

bool MCPClient::is_connected() const {
    return connected_;
}

std::vector<Tool> MCPClient::list_tools() {
    if (!connected_) {
        return {};
    }

    // Send tools/list request
    JsonRpcRequest req("tools/list", std::nullopt, generate_request_id());
    std::string response_json = send_request_sync("tools/list", "{}");

    // Parse response
    // Expected format: {"tools": [{"name": "...", "description": "...", "inputSchema": ...}, ...]}
    std::vector<Tool> tools;

    // Simple JSON parsing (extract tools array)
    auto tools_start = response_json.find("\"tools\"");
    if (tools_start == std::string::npos) {
        return tools;
    }

    auto array_start = response_json.find("[", tools_start);
    if (array_start == std::string::npos) {
        return tools;
    }

    // Parse each tool object
    size_t pos = array_start + 1;
    while (pos < response_json.length()) {
        // Skip whitespace
        while (pos < response_json.length() && std::isspace(response_json[pos])) {
            pos++;
        }

        if (pos >= response_json.length() || response_json[pos] == ']') {
            break;
        }

        // Find tool object
        if (response_json[pos] == '{') {
            size_t obj_start = pos;
            int depth = 1;
            pos++;

            while (pos < response_json.length() && depth > 0) {
                if (response_json[pos] == '{') depth++;
                else if (response_json[pos] == '}') depth--;
                pos++;
            }

            std::string tool_json = response_json.substr(obj_start, pos - obj_start);
            auto tool_opt = MessageCodec::parse_tool(tool_json);
            if (tool_opt.has_value()) {
                tools.push_back(tool_opt.value());
            }
        }

        // Skip comma
        while (pos < response_json.length() && (std::isspace(response_json[pos]) || response_json[pos] == ',')) {
            pos++;
        }
    }

    return tools;
}

ToolResult MCPClient::call_tool(const std::string& name, const std::string& params) {
    if (!connected_) {
        return ToolResult{true, "", "Not connected"};
    }

    // Build request params: {"name": "tool_name", "arguments": {...}}
    std::ostringstream oss;
    oss << "{\"name\":\"" << name << "\",\"arguments\":" << params << "}";

    std::string response_json = send_request_sync("tools/call", oss.str());

    // Parse response
    // Expected format: {"content": [{"type": "text", "text": "..."}], ...}
    auto content_start = response_json.find("\"content\"");
    if (content_start == std::string::npos) {
        return ToolResult{true, "", "Invalid response format"};
    }

    // Extract text content
    auto text_start = response_json.find("\"text\"", content_start);
    if (text_start == std::string::npos) {
        return ToolResult{true, "", "No text content in response"};
    }

    auto text_value_start = response_json.find(":", text_start) + 1;
    while (text_value_start < response_json.length() && std::isspace(response_json[text_value_start])) {
        text_value_start++;
    }

    if (response_json[text_value_start] == '"') {
        text_value_start++;
        auto text_value_end = response_json.find("\"", text_value_start);
        std::string text = response_json.substr(text_value_start, text_value_end - text_value_start);
        return ToolResult{false, text, std::nullopt};
    }

    return ToolResult{true, "", "Failed to parse response"};
}

std::future<ToolResult> MCPClient::call_tool_async(const std::string& name, const std::string& params) {
    return std::async(std::launch::async, [this, name, params]() {
        return call_tool(name, params);
    });
}

std::vector<Resource> MCPClient::list_resources() {
    if (!connected_) {
        return {};
    }

    std::string response_json = send_request_sync("resources/list", "{}");

    // Parse response: {"resources": [{"uri": "...", "name": "...", ...}, ...]}
    std::vector<Resource> resources;

    auto resources_start = response_json.find("\"resources\"");
    if (resources_start == std::string::npos) {
        return resources;
    }

    auto array_start = response_json.find("[", resources_start);
    if (array_start == std::string::npos) {
        return resources;
    }

    size_t pos = array_start + 1;
    while (pos < response_json.length()) {
        while (pos < response_json.length() && std::isspace(response_json[pos])) {
            pos++;
        }

        if (pos >= response_json.length() || response_json[pos] == ']') {
            break;
        }

        if (response_json[pos] == '{') {
            size_t obj_start = pos;
            int depth = 1;
            pos++;

            while (pos < response_json.length() && depth > 0) {
                if (response_json[pos] == '{') depth++;
                else if (response_json[pos] == '}') depth--;
                pos++;
            }

            std::string resource_json = response_json.substr(obj_start, pos - obj_start);
            auto resource_opt = MessageCodec::parse_resource(resource_json);
            if (resource_opt.has_value()) {
                resources.push_back(resource_opt.value());
            }
        }

        while (pos < response_json.length() && (std::isspace(response_json[pos]) || response_json[pos] == ',')) {
            pos++;
        }
    }

    return resources;
}

std::optional<ResourceContent> MCPClient::read_resource(const std::string& uri) {
    if (!connected_) {
        return std::nullopt;
    }

    // Build request params: {"uri": "resource_uri"}
    std::ostringstream oss;
    oss << "{\"uri\":\"" << uri << "\"}";

    std::string response_json = send_request_sync("resources/read", oss.str());

    // Parse response: {"contents": [{"uri": "...", "mimeType": "...", "text": "..."}, ...]}
    auto contents_start = response_json.find("\"contents\"");
    if (contents_start == std::string::npos) {
        return std::nullopt;
    }

    // Extract first content object
    auto obj_start = response_json.find("{", contents_start + 10);
    if (obj_start == std::string::npos) {
        return std::nullopt;
    }

    ResourceContent content;
    content.uri = uri;

    // Extract mimeType
    auto mime_start = response_json.find("\"mimeType\"", obj_start);
    if (mime_start != std::string::npos) {
        auto mime_value_start = response_json.find(":", mime_start) + 1;
        while (mime_value_start < response_json.length() && std::isspace(response_json[mime_value_start])) {
            mime_value_start++;
        }
        if (response_json[mime_value_start] == '"') {
            mime_value_start++;
            auto mime_value_end = response_json.find("\"", mime_value_start);
            content.mime_type = response_json.substr(mime_value_start, mime_value_end - mime_value_start);
        }
    }

    // Extract text content
    auto text_start = response_json.find("\"text\"", obj_start);
    if (text_start != std::string::npos) {
        auto text_value_start = response_json.find(":", text_start) + 1;
        while (text_value_start < response_json.length() && std::isspace(response_json[text_value_start])) {
            text_value_start++;
        }
        if (response_json[text_value_start] == '"') {
            text_value_start++;
            auto text_value_end = response_json.find("\"", text_value_start);
            content.content = response_json.substr(text_value_start, text_value_end - text_value_start);
        }
    }

    return content;
}

std::vector<Prompt> MCPClient::list_prompts() {
    if (!connected_) {
        return {};
    }

    std::string response_json = send_request_sync("prompts/list", "{}");

    // Parse response: {"prompts": [{"name": "...", "description": "...", ...}, ...]}
    std::vector<Prompt> prompts;

    auto prompts_start = response_json.find("\"prompts\"");
    if (prompts_start == std::string::npos) {
        return prompts;
    }

    auto array_start = response_json.find("[", prompts_start);
    if (array_start == std::string::npos) {
        return prompts;
    }

    size_t pos = array_start + 1;
    while (pos < response_json.length()) {
        while (pos < response_json.length() && std::isspace(response_json[pos])) {
            pos++;
        }

        if (pos >= response_json.length() || response_json[pos] == ']') {
            break;
        }

        if (response_json[pos] == '{') {
            size_t obj_start = pos;
            int depth = 1;
            pos++;

            while (pos < response_json.length() && depth > 0) {
                if (response_json[pos] == '{') depth++;
                else if (response_json[pos] == '}') depth--;
                pos++;
            }

            std::string prompt_json = response_json.substr(obj_start, pos - obj_start);
            auto prompt_opt = MessageCodec::parse_prompt(prompt_json);
            if (prompt_opt.has_value()) {
                prompts.push_back(prompt_opt.value());
            }
        }

        while (pos < response_json.length() && (std::isspace(response_json[pos]) || response_json[pos] == ',')) {
            pos++;
        }
    }

    return prompts;
}

std::optional<std::string> MCPClient::get_prompt(
    const std::string& name,
    const std::vector<std::string>& args
) {
    if (!connected_) {
        return std::nullopt;
    }

    // Build request params: {"name": "prompt_name", "arguments": {...}}
    std::ostringstream oss;
    oss << "{\"name\":\"" << name << "\"";
    if (!args.empty()) {
        oss << ",\"arguments\":{";
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) oss << ",";
            oss << "\"arg" << i << "\":\"" << args[i] << "\"";
        }
        oss << "}";
    }
    oss << "}";

    std::string response_json = send_request_sync("prompts/get", oss.str());

    // Parse response: {"messages": [{"role": "user", "content": {"type": "text", "text": "..."}}, ...]}
    auto messages_start = response_json.find("\"messages\"");
    if (messages_start == std::string::npos) {
        return std::nullopt;
    }

    // Extract first message text
    auto text_start = response_json.find("\"text\"", messages_start);
    if (text_start == std::string::npos) {
        return std::nullopt;
    }

    auto text_value_start = response_json.find(":", text_start) + 1;
    while (text_value_start < response_json.length() && std::isspace(response_json[text_value_start])) {
        text_value_start++;
    }

    if (response_json[text_value_start] == '"') {
        text_value_start++;
        auto text_value_end = response_json.find("\"", text_value_start);
        return response_json.substr(text_value_start, text_value_end - text_value_start);
    }

    return std::nullopt;
}

void MCPClient::handle_message(const std::string& message_str) {
    auto message_opt = MessageCodec::parse(message_str);
    if (!message_opt.has_value()) {
        return;
    }

    const auto& message = message_opt.value();

    if (std::holds_alternative<JsonRpcResponse>(message)) {
        handle_response(std::get<JsonRpcResponse>(message));
    }
    // Notifications and requests from server would be handled here
}

void MCPClient::handle_response(const JsonRpcResponse& resp) {
    std::lock_guard<std::mutex> lock(requests_mutex_);

    auto it = pending_requests_.find(resp.id);
    if (it != pending_requests_.end()) {
        // Fulfill promise
        if (resp.is_error()) {
            it->second.promise.set_value("{\"error\":\"" + resp.error->message + "\"}");
        } else {
            it->second.promise.set_value(resp.result.value_or("{}"));
        }
        pending_requests_.erase(it);
    }
}

std::string MCPClient::send_request_sync(const std::string& method, const std::string& params) {
    auto future = send_request_async(method, params);

    // Wait with timeout
    auto status = future.wait_for(std::chrono::milliseconds(config_.request_timeout_ms));
    if (status == std::future_status::timeout) {
        return "{\"error\":\"Request timeout\"}";
    }

    return future.get();
}

std::future<std::string> MCPClient::send_request_async(const std::string& method, const std::string& params) {
    std::string request_id = generate_request_id();

    // Create promise/future pair
    std::promise<std::string> promise;
    std::future<std::string> future = promise.get_future();

    // Register pending request
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        pending_requests_[request_id] = PendingRequest{
            std::move(promise),
            std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.request_timeout_ms)
        };
    }

    // Send request
    JsonRpcRequest req(method, params, request_id);
    std::string req_json = MessageCodec::serialize(JsonRpcMessage(req));
    transport_->send(req_json);

    return future;
}

std::string MCPClient::generate_request_id() {
    return std::to_string(request_counter_++);
}

} // namespace mcp
} // namespace fasterapi
