#include "session.h"
#include <random>
#include <sstream>
#include <iomanip>

namespace fasterapi {
namespace mcp {

namespace {
    // Generate a random hex string
    std::string random_hex(size_t length) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, 15);

        std::ostringstream oss;
        oss << std::hex;
        for (size_t i = 0; i < length; ++i) {
            oss << dis(gen);
        }
        return oss.str();
    }
}

Session::Session(bool is_server)
    : is_server_(is_server)
    , session_id_(generate_session_id())
    , start_time_(std::chrono::steady_clock::now())
{
}

Session::~Session() {
    if (!is_closed()) {
        state_ = SessionState::CLOSED;
    }
}

std::string Session::generate_session_id() {
    return "mcp-" + random_hex(16);
}

bool Session::transition_to(SessionState new_state) {
    SessionState current = state_.load();

    // Validate state transitions
    bool valid = false;
    switch (current) {
        case SessionState::UNINITIALIZED:
            valid = (new_state == SessionState::INITIALIZING ||
                    new_state == SessionState::ERROR);
            break;
        case SessionState::INITIALIZING:
            valid = (new_state == SessionState::READY ||
                    new_state == SessionState::ERROR ||
                    new_state == SessionState::CLOSED);
            break;
        case SessionState::READY:
            valid = (new_state == SessionState::CLOSING ||
                    new_state == SessionState::ERROR);
            break;
        case SessionState::CLOSING:
            valid = (new_state == SessionState::CLOSED);
            break;
        case SessionState::CLOSED:
        case SessionState::ERROR:
            valid = false;  // Terminal states
            break;
    }

    if (valid) {
        state_ = new_state;
    }
    return valid;
}

JsonRpcRequest Session::create_initialize_request(
    const Implementation& client_info,
    const Capabilities& client_caps
) {
    if (!transition_to(SessionState::INITIALIZING)) {
        // Cannot throw, return empty request with error marker
        JsonRpcRequest req;
        req.method = "error";
        req.params = "{\"error\":\"Invalid state for initialize request\"}";
        req.id = "error";
        return req;
    }

    InitializeRequest init_req;
    init_req.protocol_version = ProtocolVersion();
    init_req.capabilities = client_caps;
    init_req.client_info = client_info;

    // Store our info
    peer_info_ = client_info;  // Will be replaced by server info on response

    // Create JSON-RPC request
    JsonRpcRequest req;
    req.method = "initialize";
    req.params = MessageCodec::serialize(init_req);
    req.id = session_id_ + "-init";

    return req;
}

JsonRpcResponse Session::handle_initialize_request(
    const InitializeRequest& req,
    const Implementation& server_info,
    const Capabilities& server_caps
) {
    if (!transition_to(SessionState::INITIALIZING)) {
        return JsonRpcResponse::error_response(
            session_id_ + "-init",
            JsonRpcError(ErrorCode::INTERNAL_ERROR, "Invalid session state")
        );
    }

    // Store client info
    peer_info_ = req.client_info;

    // Negotiate protocol version (for now, just use ours)
    protocol_version_ = ProtocolVersion();

    // Negotiate capabilities (intersection of what both support)
    capabilities_ = server_caps;

    // Create response
    InitializeResponse init_resp;
    init_resp.protocol_version = protocol_version_;
    init_resp.capabilities = capabilities_;
    init_resp.server_info = server_info;
    init_resp.instructions = "MCP server powered by FasterAPI";

    return JsonRpcResponse::success(
        session_id_ + "-init",
        MessageCodec::serialize(init_resp)
    );
}

std::optional<JsonRpcNotification> Session::handle_initialize_response(
    const InitializeResponse& resp
) {
    // Store server info and capabilities
    peer_info_ = resp.server_info;
    protocol_version_ = resp.protocol_version;
    capabilities_ = resp.capabilities;

    // Don't transition to READY yet - wait for initialized notification

    // Create initialized notification
    JsonRpcNotification notif;
    notif.method = "notifications/initialized";
    notif.params = "{}";

    return notif;
}

void Session::handle_initialized_notification(const JsonRpcNotification& notif) {
    if (notif.method == "notifications/initialized") {
        transition_to(SessionState::READY);
    }
}

JsonRpcNotification Session::create_shutdown_notification() {
    transition_to(SessionState::CLOSING);

    JsonRpcNotification notif;
    notif.method = "notifications/shutdown";
    notif.params = "{}";

    return notif;
}

void Session::handle_shutdown_notification(const JsonRpcNotification& notif) {
    if (notif.method == "notifications/shutdown") {
        transition_to(SessionState::CLOSING);
        transition_to(SessionState::CLOSED);
    }
}

void Session::set_error(const std::string& error_msg) {
    last_error_ = error_msg;
    transition_to(SessionState::ERROR);
}

// SessionManager implementation

std::string SessionManager::create_session(bool is_server) {
    auto session = std::make_shared<Session>(is_server);
    auto session_id = session->get_session_id();

    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session_id] = session;

    return session_id;
}

std::shared_ptr<Session> SessionManager::get_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        return it->second;
    }
    return nullptr;
}

void SessionManager::remove_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session_id);
}

size_t SessionManager::get_session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::vector<std::string> SessionManager::get_session_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, _] : sessions_) {
        ids.push_back(id);
    }
    return ids;
}

void SessionManager::close_all_sessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, session] : sessions_) {
        auto notif = session->create_shutdown_notification();
        session->handle_shutdown_notification(notif);
    }
    sessions_.clear();
}

} // namespace mcp
} // namespace fasterapi
