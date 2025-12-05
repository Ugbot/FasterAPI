/**
 * Unit tests for MCP protocol layer.
 */

#include "../src/cpp/mcp/protocol/message.h"
#include "../src/cpp/mcp/protocol/session.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace fasterapi::mcp;

void test_json_rpc_request() {
    std::cout << "Testing JSON-RPC request..." << std::endl;

    JsonRpcRequest req("tools/list", "{}", "req-1");

    assert(req.method == "tools/list");
    assert(req.params.has_value());
    assert(req.id.has_value());
    assert(!req.is_notification());

    std::cout << "  ✓ Request creation and properties" << std::endl;
}

void test_json_rpc_notification() {
    std::cout << "Testing JSON-RPC notification..." << std::endl;

    JsonRpcNotification notif("notifications/initialized", "{}");

    assert(notif.method == "notifications/initialized");
    assert(notif.params.has_value());

    std::cout << "  ✓ Notification creation" << std::endl;
}

void test_json_rpc_response() {
    std::cout << "Testing JSON-RPC response..." << std::endl;

    auto success = JsonRpcResponse::success("req-1", "{\"result\": \"ok\"}");
    assert(!success.is_error());
    assert(success.result.has_value());

    auto error = JsonRpcResponse::error_response(
        "req-2",
        JsonRpcError(ErrorCode::METHOD_NOT_FOUND, "Method not found")
    );
    assert(error.is_error());
    assert(error.error.has_value());

    std::cout << "  ✓ Success and error responses" << std::endl;
}

void test_message_serialization() {
    std::cout << "Testing message serialization..." << std::endl;

    // Request
    JsonRpcRequest req("test", "{\"param\": 1}", "1");
    JsonRpcMessage msg = req;
    std::string json = MessageCodec::serialize(msg);

    assert(json.find("\"method\":\"test\"") != std::string::npos);
    assert(json.find("\"id\":\"1\"") != std::string::npos);

    // Response
    auto resp = JsonRpcResponse::success("2", "{\"data\": \"value\"}");
    JsonRpcMessage msg2 = resp;
    std::string json2 = MessageCodec::serialize(msg2);

    assert(json2.find("\"result\"") != std::string::npos);
    assert(json2.find("\"id\":\"2\"") != std::string::npos);

    std::cout << "  ✓ Request and response serialization" << std::endl;
}

void test_message_parsing() {
    std::cout << "Testing message parsing..." << std::endl;

    // Parse request
    std::string req_json = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\",\"id\":\"1\"}";
    auto msg_opt = MessageCodec::parse(req_json);

    assert(msg_opt.has_value());
    assert(std::holds_alternative<JsonRpcRequest>(msg_opt.value()));

    auto& req = std::get<JsonRpcRequest>(msg_opt.value());
    assert(req.method == "tools/list");
    assert(req.id == "1");

    // Parse notification
    std::string notif_json = "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\"}";
    auto msg_opt2 = MessageCodec::parse(notif_json);

    assert(msg_opt2.has_value());
    assert(std::holds_alternative<JsonRpcNotification>(msg_opt2.value()));

    std::cout << "  ✓ Request and notification parsing" << std::endl;
}

void test_session_lifecycle() {
    std::cout << "Testing session lifecycle..." << std::endl;

    Session session(false);  // Client session

    assert(session.get_state() == SessionState::UNINITIALIZED);
    assert(!session.is_ready());
    assert(!session.is_closed());

    // Create initialize request
    Implementation client_info;
    client_info.name = "Test Client";

    Capabilities caps;
    caps.client.sampling = true;

    auto init_req = session.create_initialize_request(client_info, caps);
    assert(init_req.method == "initialize");

    std::cout << "  ✓ Session creation and initialization" << std::endl;
}

void test_session_state_transitions() {
    std::cout << "Testing session state transitions..." << std::endl;

    Session server_session(true);

    // Initialize
    InitializeRequest init_req;
    init_req.client_info.name = "Client";

    Implementation server_info;
    server_info.name = "Server";

    Capabilities server_caps;
    server_caps.server.tools = true;

    auto resp = server_session.handle_initialize_request(init_req, server_info, server_caps);
    assert(!resp.is_error());

    // Handle initialized notification
    JsonRpcNotification notif("notifications/initialized", "{}");
    server_session.handle_initialized_notification(notif);

    assert(server_session.is_ready());

    // Shutdown
    auto shutdown = server_session.create_shutdown_notification();
    server_session.handle_shutdown_notification(shutdown);

    assert(server_session.is_closed());

    std::cout << "  ✓ State transitions (init → ready → closed)" << std::endl;
}

void test_session_manager() {
    std::cout << "Testing session manager..." << std::endl;

    SessionManager manager;

    auto id1 = manager.create_session(true);
    auto id2 = manager.create_session(false);

    assert(manager.get_session_count() == 2);

    auto session1 = manager.get_session(id1);
    assert(session1 != nullptr);
    assert(session1->is_server());

    auto session2 = manager.get_session(id2);
    assert(session2 != nullptr);
    assert(!session2->is_server());

    manager.remove_session(id1);
    assert(manager.get_session_count() == 1);

    manager.close_all_sessions();
    assert(manager.get_session_count() == 0);

    std::cout << "  ✓ Session manager create/get/remove" << std::endl;
}

void test_tool_definition() {
    std::cout << "Testing tool definition..." << std::endl;

    Tool tool;
    tool.name = "calculate";
    tool.description = "Perform calculation";
    tool.input_schema = "{\"type\":\"object\"}";

    std::string json = MessageCodec::serialize(tool);

    assert(json.find("\"name\":\"calculate\"") != std::string::npos);
    assert(json.find("\"description\"") != std::string::npos);
    assert(json.find("\"inputSchema\"") != std::string::npos);

    std::cout << "  ✓ Tool definition and serialization" << std::endl;
}

void test_resource_definition() {
    std::cout << "Testing resource definition..." << std::endl;

    Resource resource;
    resource.uri = "file:///config.json";
    resource.name = "Configuration";
    resource.description = "App config";
    resource.mime_type = "application/json";

    std::string json = MessageCodec::serialize(resource);

    assert(json.find("\"uri\":\"file:///config.json\"") != std::string::npos);
    assert(json.find("\"name\":\"Configuration\"") != std::string::npos);

    std::cout << "  ✓ Resource definition and serialization" << std::endl;
}

void test_error_codes() {
    std::cout << "Testing error codes..." << std::endl;

    JsonRpcError parse_err(ErrorCode::PARSE_ERROR, "Parse failed");
    assert(static_cast<int>(parse_err.code) == -32700);

    JsonRpcError method_err(ErrorCode::METHOD_NOT_FOUND, "Not found");
    assert(static_cast<int>(method_err.code) == -32601);

    JsonRpcError auth_err(ErrorCode::UNAUTHORIZED, "Unauthorized");
    assert(static_cast<int>(auth_err.code) == -32001);

    std::cout << "  ✓ Error code values" << std::endl;
}

int main() {
    std::cout << "\n=== MCP Protocol Tests ===\n" << std::endl;

    try {
        test_json_rpc_request();
        test_json_rpc_notification();
        test_json_rpc_response();
        test_message_serialization();
        test_message_parsing();
        test_session_lifecycle();
        test_session_state_transitions();
        test_session_manager();
        test_tool_definition();
        test_resource_definition();
        test_error_codes();

        std::cout << "\n✅ All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
