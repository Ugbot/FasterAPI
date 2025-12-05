#include "message.h"
#include <sstream>
#include <iomanip>

// Note: We'll use a simple JSON implementation for now
// TODO: Integrate simdjson for production performance

namespace fasterapi {
namespace mcp {

namespace {
    // Helper: Escape JSON string
    std::string json_escape(const std::string& str) {
        std::ostringstream oss;
        for (char c : str) {
            switch (c) {
                case '"': oss << "\\\""; break;
                case '\\': oss << "\\\\"; break;
                case '\b': oss << "\\b"; break;
                case '\f': oss << "\\f"; break;
                case '\n': oss << "\\n"; break;
                case '\r': oss << "\\r"; break;
                case '\t': oss << "\\t"; break;
                default:
                    if (c >= 0 && c < 32) {
                        oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                    } else {
                        oss << c;
                    }
            }
        }
        return oss.str();
    }

    // Helper: Basic JSON parsing (extract field value)
    std::optional<std::string> extract_field(const std::string& json, const std::string& field) {
        std::string search = "\"" + field + "\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) return std::nullopt;

        // Find the colon
        pos = json.find(':', pos);
        if (pos == std::string::npos) return std::nullopt;

        // Skip whitespace
        pos++;
        while (pos < json.length() && std::isspace(json[pos])) pos++;

        // Check if it's a string, object, array, or primitive
        if (json[pos] == '"') {
            // String value
            pos++;
            auto end = json.find('"', pos);
            if (end == std::string::npos) return std::nullopt;
            return json.substr(pos, end - pos);
        } else if (json[pos] == '{') {
            // Object value
            int depth = 1;
            auto start = pos;
            pos++;
            while (pos < json.length() && depth > 0) {
                if (json[pos] == '{') depth++;
                else if (json[pos] == '}') depth--;
                pos++;
            }
            return json.substr(start, pos - start);
        } else if (json[pos] == '[') {
            // Array value
            int depth = 1;
            auto start = pos;
            pos++;
            while (pos < json.length() && depth > 0) {
                if (json[pos] == '[') depth++;
                else if (json[pos] == ']') depth--;
                pos++;
            }
            return json.substr(start, pos - start);
        } else {
            // Primitive value (number, boolean, null)
            auto start = pos;
            while (pos < json.length() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']') {
                pos++;
            }
            auto value = json.substr(start, pos - start);
            // Trim whitespace
            value.erase(value.find_last_not_of(" \n\r\t") + 1);
            return value;
        }
    }
}

std::optional<JsonRpcMessage> MessageCodec::parse(const std::string& json) noexcept {
    // Determine message type by presence of fields
    auto method = extract_field(json, "method");
    auto id = extract_field(json, "id");
    auto result = extract_field(json, "result");
    auto error = extract_field(json, "error");

    if (result.has_value() || error.has_value()) {
        // This is a response
        JsonRpcResponse resp;
        resp.id = id.value_or("");
        resp.result = result;

        if (error.has_value()) {
            // Parse error object
            auto code_str = extract_field(error.value(), "code");
            auto message = extract_field(error.value(), "message");
            auto data = extract_field(error.value(), "data");

            if (code_str.has_value() && message.has_value()) {
                // std::stoi could throw but we're marked noexcept
                // Do manual parsing instead
                int code_int = 0;
                const char* str = code_str.value().c_str();
                bool negative = (*str == '-');
                if (negative) ++str;
                while (*str >= '0' && *str <= '9') {
                    code_int = code_int * 10 + (*str - '0');
                    ++str;
                }
                if (negative) code_int = -code_int;

                resp.error = JsonRpcError(
                    static_cast<ErrorCode>(code_int),
                    message.value(),
                    data
                );
            }
        }

        return resp;

    } else if (method.has_value()) {
        auto params = extract_field(json, "params");

        if (id.has_value()) {
            // This is a request
            JsonRpcRequest req;
            req.method = method.value();
            req.params = params;
            req.id = id;
            return req;
        } else {
            // This is a notification
            JsonRpcNotification notif;
            notif.method = method.value();
            notif.params = params;
            return notif;
        }
    }

    return std::nullopt;
}

std::string MessageCodec::serialize(const JsonRpcMessage& msg) noexcept {
    if (std::holds_alternative<JsonRpcRequest>(msg)) {
        const auto& req = std::get<JsonRpcRequest>(msg);
        std::ostringstream oss;
        oss << "{\"jsonrpc\":\"2.0\",\"method\":\"" << json_escape(req.method) << "\"";
        if (req.params.has_value()) {
            oss << ",\"params\":" << req.params.value();
        }
        if (req.id.has_value()) {
            oss << ",\"id\":\"" << json_escape(req.id.value()) << "\"";
        }
        oss << "}";
        return oss.str();

    } else if (std::holds_alternative<JsonRpcResponse>(msg)) {
        const auto& resp = std::get<JsonRpcResponse>(msg);
        std::ostringstream oss;
        oss << "{\"jsonrpc\":\"2.0\",\"id\":\"" << json_escape(resp.id) << "\"";

        if (resp.error.has_value()) {
            const auto& err = resp.error.value();
            oss << ",\"error\":{\"code\":" << static_cast<int>(err.code)
                << ",\"message\":\"" << json_escape(err.message) << "\"";
            if (err.data.has_value()) {
                oss << ",\"data\":\"" << json_escape(err.data.value()) << "\"";
            }
            oss << "}";
        } else if (resp.result.has_value()) {
            oss << ",\"result\":" << resp.result.value();
        } else {
            oss << ",\"result\":null";
        }

        oss << "}";
        return oss.str();

    } else if (std::holds_alternative<JsonRpcNotification>(msg)) {
        const auto& notif = std::get<JsonRpcNotification>(msg);
        std::ostringstream oss;
        oss << "{\"jsonrpc\":\"2.0\",\"method\":\"" << json_escape(notif.method) << "\"";
        if (notif.params.has_value()) {
            oss << ",\"params\":" << notif.params.value();
        }
        oss << "}";
        return oss.str();
    }

    return "{}";
}

std::optional<InitializeRequest> MessageCodec::parse_initialize_request(const std::string& json) noexcept {
    InitializeRequest req;

    // Extract protocol version
    auto version = extract_field(json, "protocolVersion");
    if (version.has_value()) {
        req.protocol_version.version = version.value();
    }

    // Extract capabilities
    auto caps = extract_field(json, "capabilities");
    if (caps.has_value()) {
        // Parse server capabilities if present
        auto tools = extract_field(caps.value(), "tools");
        auto resources = extract_field(caps.value(), "resources");
        auto prompts = extract_field(caps.value(), "prompts");

        req.capabilities.server.tools = (tools.has_value());
        req.capabilities.server.resources = (resources.has_value());
        req.capabilities.server.prompts = (prompts.has_value());
    }

    // Extract client info
    auto client_info = extract_field(json, "clientInfo");
    if (client_info.has_value()) {
        auto name = extract_field(client_info.value(), "name");
        auto ver = extract_field(client_info.value(), "version");
        if (name.has_value()) req.client_info.name = name.value();
        if (ver.has_value()) req.client_info.version = ver.value();
    }

    return req;
}

std::string MessageCodec::serialize(const InitializeRequest& req) noexcept {
    std::ostringstream oss;
    oss << "{"
        << "\"protocolVersion\":\"" << json_escape(req.protocol_version.version) << "\","
        << "\"capabilities\":{"
        << "\"experimental\":{},"
        << "\"sampling\":{},"
        << "\"roots\":{\"listChanged\":false}"
        << "},"
        << "\"clientInfo\":{"
        << "\"name\":\"" << json_escape(req.client_info.name) << "\","
        << "\"version\":\"" << json_escape(req.client_info.version) << "\""
        << "}"
        << "}";
    return oss.str();
}

std::string MessageCodec::serialize(const InitializeResponse& resp) noexcept {
    std::ostringstream oss;
    oss << "{"
        << "\"protocolVersion\":\"" << json_escape(resp.protocol_version.version) << "\","
        << "\"capabilities\":{"
        << "\"tools\":{\"listChanged\":" << (resp.capabilities.server.tools_list_changed ? "true" : "false") << "},"
        << "\"resources\":{\"listChanged\":" << (resp.capabilities.server.resources_list_changed ? "true" : "false") << "},"
        << "\"prompts\":{\"listChanged\":" << (resp.capabilities.server.prompts_list_changed ? "true" : "false") << "},"
        << "\"logging\":{},"
        << "\"experimental\":{}"
        << "},"
        << "\"serverInfo\":{"
        << "\"name\":\"" << json_escape(resp.server_info.name) << "\","
        << "\"version\":\"" << json_escape(resp.server_info.version) << "\""
        << "}";

    if (resp.instructions.has_value()) {
        oss << ",\"instructions\":\"" << json_escape(resp.instructions.value()) << "\"";
    }

    oss << "}";
    return oss.str();
}

std::string MessageCodec::serialize(const Tool& tool) noexcept {
    std::ostringstream oss;
    oss << "{"
        << "\"name\":\"" << json_escape(tool.name) << "\","
        << "\"description\":\"" << json_escape(tool.description) << "\"";

    if (tool.input_schema.has_value()) {
        oss << ",\"inputSchema\":" << tool.input_schema.value();
    }

    oss << "}";
    return oss.str();
}

std::string MessageCodec::serialize(const Resource& resource) noexcept {
    std::ostringstream oss;
    oss << "{"
        << "\"uri\":\"" << json_escape(resource.uri) << "\","
        << "\"name\":\"" << json_escape(resource.name) << "\"";

    if (resource.description.has_value()) {
        oss << ",\"description\":\"" << json_escape(resource.description.value()) << "\"";
    }
    if (resource.mime_type.has_value()) {
        oss << ",\"mimeType\":\"" << json_escape(resource.mime_type.value()) << "\"";
    }

    oss << "}";
    return oss.str();
}

std::string MessageCodec::serialize(const Prompt& prompt) noexcept {
    std::ostringstream oss;
    oss << "{"
        << "\"name\":\"" << json_escape(prompt.name) << "\","
        << "\"description\":\"" << json_escape(prompt.description) << "\"";

    if (prompt.arguments.has_value() && !prompt.arguments.value().empty()) {
        oss << ",\"arguments\":[";
        bool first = true;
        for (const auto& arg : prompt.arguments.value()) {
            if (!first) oss << ",";
            oss << "\"" << json_escape(arg) << "\"";
            first = false;
        }
        oss << "]";
    }

    oss << "}";
    return oss.str();
}

std::string MessageCodec::create_error_response(const std::string& id, ErrorCode code, const std::string& message) noexcept {
    std::ostringstream oss;
    oss << "{"
        << "\"jsonrpc\":\"2.0\","
        << "\"id\":\"" << json_escape(id) << "\","
        << "\"error\":{"
        << "\"code\":" << static_cast<int>(code) << ","
        << "\"message\":\"" << json_escape(message) << "\""
        << "}"
        << "}";
    return oss.str();
}

} // namespace mcp
} // namespace fasterapi
