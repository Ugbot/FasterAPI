#pragma once

#include "message.h"
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>

namespace fasterapi {
namespace mcp {

/**
 * MCP session state machine
 */
enum class SessionState {
    UNINITIALIZED,  // Session created but not initialized
    INITIALIZING,   // Initialize request sent, waiting for response
    READY,          // Session initialized and ready
    CLOSING,        // Close requested
    CLOSED,         // Session closed
    ERROR           // Session in error state
};

/**
 * MCP session manages the lifecycle of a client-server connection.
 *
 * Protocol flow:
 * 1. Client sends initialize request
 * 2. Server responds with initialize response
 * 3. Client sends initialized notification
 * 4. Session is READY
 * 5. Exchange tools/resources/prompts messages
 * 6. Client sends shutdown notification (optional)
 * 7. Session CLOSED
 */
class Session {
public:
    using MessageHandler = std::function<void(const JsonRpcMessage&)>;
    using ErrorHandler = std::function<void(const std::string&)>;

    Session(bool is_server = false);
    ~Session();

    // Non-copyable, movable
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;

    /**
     * Initialize the session (client side).
     *
     * @param client_info Client implementation info
     * @param client_caps Client capabilities
     * @return Initialize request message to send
     */
    JsonRpcRequest create_initialize_request(
        const Implementation& client_info,
        const Capabilities& client_caps
    );

    /**
     * Handle initialize request (server side).
     *
     * @param req Initialize request from client
     * @param server_info Server implementation info
     * @param server_caps Server capabilities
     * @return Initialize response message to send
     */
    JsonRpcResponse handle_initialize_request(
        const InitializeRequest& req,
        const Implementation& server_info,
        const Capabilities& server_caps
    );

    /**
     * Handle initialize response (client side).
     *
     * @param resp Initialize response from server
     * @return Initialized notification to send, or nullopt on error
     */
    std::optional<JsonRpcNotification> handle_initialize_response(
        const InitializeResponse& resp
    );

    /**
     * Handle initialized notification (server side).
     *
     * @param notif Initialized notification from client
     */
    void handle_initialized_notification(const JsonRpcNotification& notif);

    /**
     * Create a shutdown notification.
     *
     * @return Shutdown notification message
     */
    JsonRpcNotification create_shutdown_notification();

    /**
     * Handle shutdown notification.
     *
     * @param notif Shutdown notification
     */
    void handle_shutdown_notification(const JsonRpcNotification& notif);

    /**
     * Get current session state.
     */
    SessionState get_state() const { return state_; }

    /**
     * Check if session is ready for normal operations.
     */
    bool is_ready() const { return state_ == SessionState::READY; }

    /**
     * Check if session is closed.
     */
    bool is_closed() const {
        return state_ == SessionState::CLOSED || state_ == SessionState::ERROR;
    }

    /**
     * Get negotiated protocol version.
     */
    const ProtocolVersion& get_protocol_version() const { return protocol_version_; }

    /**
     * Get negotiated capabilities.
     */
    const Capabilities& get_capabilities() const { return capabilities_; }

    /**
     * Get peer implementation info.
     */
    const Implementation& get_peer_info() const { return peer_info_; }

    /**
     * Get session ID (unique identifier).
     */
    const std::string& get_session_id() const { return session_id_; }

    /**
     * Check if this is a server session.
     */
    bool is_server() const { return is_server_; }

    /**
     * Set error state with message.
     */
    void set_error(const std::string& error_msg);

    /**
     * Get last error message.
     */
    const std::string& get_error() const { return last_error_; }

    /**
     * Get session start time.
     */
    std::chrono::steady_clock::time_point get_start_time() const {
        return start_time_;
    }

    /**
     * Get session duration.
     */
    std::chrono::milliseconds get_duration() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_
        );
    }

private:
    std::atomic<SessionState> state_{SessionState::UNINITIALIZED};
    bool is_server_;
    std::string session_id_;

    // Negotiated session parameters
    ProtocolVersion protocol_version_;
    Capabilities capabilities_;
    Implementation peer_info_;

    // Error tracking
    std::string last_error_;

    // Timing
    std::chrono::steady_clock::time_point start_time_;

    // Generate unique session ID
    static std::string generate_session_id();

    // Update state with validation
    bool transition_to(SessionState new_state);
};

/**
 * Session manager for tracking multiple active sessions.
 */
class SessionManager {
public:
    SessionManager() = default;
    ~SessionManager() = default;

    /**
     * Create a new session.
     *
     * @param is_server Is this a server session?
     * @return Session ID
     */
    std::string create_session(bool is_server = false);

    /**
     * Get session by ID.
     *
     * @param session_id Session ID
     * @return Shared pointer to session, or nullptr if not found
     */
    std::shared_ptr<Session> get_session(const std::string& session_id);

    /**
     * Remove session.
     *
     * @param session_id Session ID
     */
    void remove_session(const std::string& session_id);

    /**
     * Get number of active sessions.
     */
    size_t get_session_count() const;

    /**
     * Get all session IDs.
     */
    std::vector<std::string> get_session_ids() const;

    /**
     * Close all sessions.
     */
    void close_all_sessions();

private:
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    mutable std::mutex mutex_;
};

} // namespace mcp
} // namespace fasterapi
