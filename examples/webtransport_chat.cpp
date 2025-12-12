/**
 * WebTransport Chat Example - Real-time chat using WebTransport (RFC 9297)
 *
 * Demonstrates:
 * - WebTransport session management
 * - Bidirectional streams for reliable chat messages
 * - Datagrams for unreliable typing indicators
 * - Multi-client broadcast
 * - Room-based chat
 * - User presence tracking
 *
 * WebTransport advantages over WebSocket:
 * - Multiple streams per connection (no head-of-line blocking)
 * - Unreliable datagrams (perfect for ephemeral data like typing)
 * - Native multiplexing
 * - Better performance on lossy networks
 *
 * Build:
 *   cmake --build build --target webtransport_chat
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/webtransport_chat
 *
 * Note: Requires HTTP/3 client with WebTransport support (e.g., Chrome with flags)
 */

#include "../src/cpp/http/webtransport_connection.h"
#include "../src/cpp/http/quic/quic_connection.h"
#include "../src/cpp/core/logger.h"

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <random>

using namespace fasterapi::http;
using namespace fasterapi::quic;

// =============================================================================
// Message Types
// =============================================================================

enum class MessageType : uint8_t {
    // Reliable messages (via streams)
    CHAT_MESSAGE = 0x01,      // User chat message
    JOIN_ROOM = 0x02,         // Join a room
    LEAVE_ROOM = 0x03,        // Leave a room
    USER_LIST = 0x04,         // List of users in room
    SYSTEM_MESSAGE = 0x05,    // System notification

    // Unreliable messages (via datagrams)
    TYPING_START = 0x10,      // User started typing
    TYPING_STOP = 0x11,       // User stopped typing
    PRESENCE_PING = 0x12,     // Keep-alive / presence update
};

// =============================================================================
// Chat User
// =============================================================================

struct ChatUser {
    uint64_t id;
    std::string username;
    std::string current_room;
    WebTransportConnection* connection;
    uint64_t main_stream_id;  // Primary bidirectional stream for this user
    std::chrono::steady_clock::time_point last_activity;
    bool is_typing;

    ChatUser() : id(0), connection(nullptr), main_stream_id(0), is_typing(false) {}
};

// =============================================================================
// Chat Room
// =============================================================================

struct ChatRoom {
    std::string name;
    std::unordered_set<uint64_t> user_ids;
    std::vector<std::pair<std::string, std::string>> message_history;  // (username, message)
    static constexpr size_t MAX_HISTORY = 50;

    void add_message(const std::string& username, const std::string& message) {
        if (message_history.size() >= MAX_HISTORY) {
            message_history.erase(message_history.begin());
        }
        message_history.emplace_back(username, message);
    }
};

// =============================================================================
// Chat Server
// =============================================================================

class ChatServer {
public:
    ChatServer() : next_user_id_(1) {
        // Create default room
        rooms_["general"] = ChatRoom{"general", {}, {}};
        rooms_["random"] = ChatRoom{"random", {}, {}};
        rooms_["tech"] = ChatRoom{"tech", {}, {}};
    }

    ~ChatServer() {
        // Clear all connections first (before users_ is destroyed)
        // This prevents callbacks from referencing destroyed user data
        clear();
    }

    // Clear all connections and users
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Clear callbacks before destroying connections
        for (auto& [id, conn] : connections_) {
            if (conn) {
                conn->on_stream_data(nullptr);
                conn->on_datagram(nullptr);
                conn->on_stream_opened(nullptr);
                conn->on_connection_closed(nullptr);
            }
        }
        connections_.clear();
        users_.clear();
        for (auto& [name, room] : rooms_) {
            room.user_ids.clear();
        }
    }

    // Handle new WebTransport connection
    void on_connection(std::unique_ptr<WebTransportConnection> wt) {
        std::lock_guard<std::mutex> lock(mutex_);

        uint64_t user_id = next_user_id_++;
        std::string default_username = "User" + std::to_string(user_id);

        // Create user
        auto& user = users_[user_id];
        user.id = user_id;
        user.username = default_username;
        user.current_room = "general";
        user.connection = wt.get();
        user.last_activity = std::chrono::steady_clock::now();
        user.is_typing = false;

        // Store connection
        connections_[user_id] = std::move(wt);

        // Setup callbacks
        auto* conn = user.connection;

        conn->on_stream_data([this, user_id](uint64_t stream_id, const uint8_t* data, size_t len) {
            handle_stream_data(user_id, stream_id, data, len);
        });

        conn->on_datagram([this, user_id](const uint8_t* data, size_t len) {
            handle_datagram(user_id, data, len);
        });

        conn->on_stream_opened([this, user_id](uint64_t stream_id, bool is_bidi) {
            handle_stream_opened(user_id, stream_id, is_bidi);
        });

        conn->on_connection_closed([this, user_id](uint64_t code, const char* reason) {
            handle_disconnect(user_id, code, reason);
        });

        // Join default room
        rooms_["general"].user_ids.insert(user_id);

        // Send welcome message
        send_system_message(user_id, "Welcome to FasterAPI Chat!");
        send_system_message(user_id, "You are " + user.username);
        send_system_message(user_id, "Type /help for commands");

        // Notify room
        broadcast_to_room("general", user.username + " joined the room", user_id);

        std::cout << "[Chat] User " << user.username << " connected (id=" << user_id << ")" << std::endl;
    }

    // Process one tick (call in event loop)
    void tick() {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();

        // Check for idle users (presence timeout)
        for (auto& [user_id, user] : users_) {
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                now - user.last_activity).count();

            if (idle > 60) {
                // Send presence request
                send_presence_ping(user_id);
            }
        }
    }

private:
    std::mutex mutex_;
    std::atomic<uint64_t> next_user_id_;
    std::unordered_map<uint64_t, ChatUser> users_;
    std::unordered_map<uint64_t, std::unique_ptr<WebTransportConnection>> connections_;
    std::unordered_map<std::string, ChatRoom> rooms_;

    // Handle incoming stream data (reliable messages)
    void handle_stream_data(uint64_t user_id, uint64_t stream_id, const uint8_t* data, size_t len) {
        if (len < 1) return;

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = users_.find(user_id);
        if (it == users_.end()) return;

        auto& user = it->second;
        user.last_activity = std::chrono::steady_clock::now();

        MessageType type = static_cast<MessageType>(data[0]);

        switch (type) {
            case MessageType::CHAT_MESSAGE:
                handle_chat_message(user, data + 1, len - 1);
                break;
            case MessageType::JOIN_ROOM:
                handle_join_room(user, data + 1, len - 1);
                break;
            case MessageType::LEAVE_ROOM:
                handle_leave_room(user);
                break;
            default:
                break;
        }
    }

    // Handle incoming datagram (unreliable messages)
    void handle_datagram(uint64_t user_id, const uint8_t* data, size_t len) {
        if (len < 1) return;

        std::lock_guard<std::mutex> lock(mutex_);

        auto it = users_.find(user_id);
        if (it == users_.end()) return;

        auto& user = it->second;

        MessageType type = static_cast<MessageType>(data[0]);

        switch (type) {
            case MessageType::TYPING_START:
                user.is_typing = true;
                broadcast_typing(user, true);
                break;
            case MessageType::TYPING_STOP:
                user.is_typing = false;
                broadcast_typing(user, false);
                break;
            case MessageType::PRESENCE_PING:
                user.last_activity = std::chrono::steady_clock::now();
                break;
            default:
                break;
        }
    }

    // Handle new stream opened by peer
    void handle_stream_opened(uint64_t user_id, uint64_t stream_id, bool is_bidi) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = users_.find(user_id);
        if (it == users_.end()) return;

        auto& user = it->second;

        // Use first bidirectional stream as main stream
        if (is_bidi && user.main_stream_id == 0) {
            user.main_stream_id = stream_id;
            std::cout << "[Chat] User " << user.username << " main stream: " << stream_id << std::endl;
        }
    }

    // Handle user disconnect
    void handle_disconnect(uint64_t user_id, uint64_t code, const char* reason) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = users_.find(user_id);
        if (it == users_.end()) return;

        auto& user = it->second;
        std::string room = user.current_room;
        std::string username = user.username;

        // Remove from room
        if (!room.empty()) {
            auto room_it = rooms_.find(room);
            if (room_it != rooms_.end()) {
                room_it->second.user_ids.erase(user_id);
            }
        }

        // Remove user and connection
        users_.erase(it);
        connections_.erase(user_id);

        // Notify room
        if (!room.empty()) {
            broadcast_to_room(room, username + " left the room");
        }

        std::cout << "[Chat] User " << username << " disconnected"
                  << " (code=" << code << ", reason=" << (reason ? reason : "none") << ")"
                  << std::endl;
    }

    // Handle chat message
    void handle_chat_message(ChatUser& user, const uint8_t* data, size_t len) {
        std::string message(reinterpret_cast<const char*>(data), len);

        // Check for commands
        if (!message.empty() && message[0] == '/') {
            handle_command(user, message.substr(1));
            return;
        }

        // Stop typing indicator
        user.is_typing = false;

        // Add to room history
        auto room_it = rooms_.find(user.current_room);
        if (room_it != rooms_.end()) {
            room_it->second.add_message(user.username, message);
        }

        // Broadcast to room
        broadcast_chat_message(user.current_room, user.username, message);

        std::cout << "[" << user.current_room << "] " << user.username << ": " << message << std::endl;
    }

    // Handle join room command
    void handle_join_room(ChatUser& user, const uint8_t* data, size_t len) {
        std::string new_room(reinterpret_cast<const char*>(data), len);

        // Leave current room
        if (!user.current_room.empty()) {
            auto old_room_it = rooms_.find(user.current_room);
            if (old_room_it != rooms_.end()) {
                old_room_it->second.user_ids.erase(user.id);
                broadcast_to_room(user.current_room, user.username + " left the room", user.id);
            }
        }

        // Create room if doesn't exist
        if (rooms_.find(new_room) == rooms_.end()) {
            rooms_[new_room] = ChatRoom{new_room, {}, {}};
        }

        // Join new room
        user.current_room = new_room;
        rooms_[new_room].user_ids.insert(user.id);

        // Notify
        send_system_message(user.id, "Joined room: " + new_room);
        broadcast_to_room(new_room, user.username + " joined the room", user.id);

        // Send user list
        send_user_list(user.id, new_room);
    }

    // Handle leave room
    void handle_leave_room(ChatUser& user) {
        if (user.current_room.empty()) return;

        auto room_it = rooms_.find(user.current_room);
        if (room_it != rooms_.end()) {
            room_it->second.user_ids.erase(user.id);
            broadcast_to_room(user.current_room, user.username + " left the room", user.id);
        }

        user.current_room = "";
        send_system_message(user.id, "Left room");
    }

    // Handle slash commands
    void handle_command(ChatUser& user, const std::string& cmd) {
        if (cmd == "help") {
            send_system_message(user.id, "Commands:");
            send_system_message(user.id, "  /help - Show this help");
            send_system_message(user.id, "  /nick <name> - Change nickname");
            send_system_message(user.id, "  /join <room> - Join a room");
            send_system_message(user.id, "  /rooms - List rooms");
            send_system_message(user.id, "  /users - List users in room");
        } else if (cmd.substr(0, 5) == "nick ") {
            std::string new_nick = cmd.substr(5);
            std::string old_nick = user.username;
            user.username = new_nick;
            send_system_message(user.id, "You are now known as " + new_nick);
            broadcast_to_room(user.current_room, old_nick + " is now known as " + new_nick, user.id);
        } else if (cmd == "rooms") {
            send_system_message(user.id, "Available rooms:");
            for (const auto& [name, room] : rooms_) {
                send_system_message(user.id, "  #" + name + " (" + std::to_string(room.user_ids.size()) + " users)");
            }
        } else if (cmd == "users") {
            send_user_list(user.id, user.current_room);
        } else if (cmd.substr(0, 5) == "join ") {
            std::string room_name = cmd.substr(5);
            std::vector<uint8_t> data;
            data.push_back(static_cast<uint8_t>(MessageType::JOIN_ROOM));
            data.insert(data.end(), room_name.begin(), room_name.end());
            handle_join_room(user, data.data() + 1, data.size() - 1);
        } else {
            send_system_message(user.id, "Unknown command: /" + cmd);
        }
    }

    // Send system message to user
    void send_system_message(uint64_t user_id, const std::string& message) {
        auto it = users_.find(user_id);
        if (it == users_.end()) return;

        auto& user = it->second;
        if (!user.connection || user.main_stream_id == 0) return;

        std::vector<uint8_t> data;
        data.push_back(static_cast<uint8_t>(MessageType::SYSTEM_MESSAGE));
        data.insert(data.end(), message.begin(), message.end());

        user.connection->send_stream(user.main_stream_id, data.data(), data.size());
    }

    // Send user list to user
    void send_user_list(uint64_t user_id, const std::string& room) {
        auto it = users_.find(user_id);
        if (it == users_.end()) return;

        auto room_it = rooms_.find(room);
        if (room_it == rooms_.end()) return;

        std::ostringstream ss;
        ss << "Users in #" << room << ":";

        for (uint64_t uid : room_it->second.user_ids) {
            auto user_it = users_.find(uid);
            if (user_it != users_.end()) {
                ss << " " << user_it->second.username;
                if (user_it->second.is_typing) {
                    ss << " (typing)";
                }
            }
        }

        send_system_message(user_id, ss.str());
    }

    // Broadcast message to room
    void broadcast_to_room(const std::string& room, const std::string& message, uint64_t exclude_id = 0) {
        auto room_it = rooms_.find(room);
        if (room_it == rooms_.end()) return;

        for (uint64_t user_id : room_it->second.user_ids) {
            if (user_id != exclude_id) {
                send_system_message(user_id, message);
            }
        }
    }

    // Broadcast chat message to room
    void broadcast_chat_message(const std::string& room, const std::string& username, const std::string& message) {
        auto room_it = rooms_.find(room);
        if (room_it == rooms_.end()) return;

        std::vector<uint8_t> data;
        data.push_back(static_cast<uint8_t>(MessageType::CHAT_MESSAGE));

        // Format: username_len(1) + username + message
        data.push_back(static_cast<uint8_t>(username.size()));
        data.insert(data.end(), username.begin(), username.end());
        data.insert(data.end(), message.begin(), message.end());

        for (uint64_t user_id : room_it->second.user_ids) {
            auto it = users_.find(user_id);
            if (it != users_.end() && it->second.connection && it->second.main_stream_id != 0) {
                it->second.connection->send_stream(it->second.main_stream_id, data.data(), data.size());
            }
        }
    }

    // Broadcast typing indicator (via datagram - unreliable)
    void broadcast_typing(const ChatUser& typing_user, bool is_typing) {
        auto room_it = rooms_.find(typing_user.current_room);
        if (room_it == rooms_.end()) return;

        std::vector<uint8_t> data;
        data.push_back(static_cast<uint8_t>(is_typing ? MessageType::TYPING_START : MessageType::TYPING_STOP));
        data.push_back(static_cast<uint8_t>(typing_user.username.size()));
        data.insert(data.end(), typing_user.username.begin(), typing_user.username.end());

        for (uint64_t user_id : room_it->second.user_ids) {
            if (user_id != typing_user.id) {
                auto it = users_.find(user_id);
                if (it != users_.end() && it->second.connection) {
                    // Datagrams are unreliable - OK if they're dropped
                    it->second.connection->send_datagram(data.data(), data.size());
                }
            }
        }
    }

    // Send presence ping
    void send_presence_ping(uint64_t user_id) {
        auto it = users_.find(user_id);
        if (it == users_.end() || !it->second.connection) return;

        uint8_t data[] = {static_cast<uint8_t>(MessageType::PRESENCE_PING)};
        it->second.connection->send_datagram(data, sizeof(data));
    }
};

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== WebTransport Chat Server ===" << std::endl;
    std::cout << std::endl;
    std::cout << "This example demonstrates WebTransport (RFC 9297) for chat:" << std::endl;
    std::cout << "  - Bidirectional streams: Reliable chat messages" << std::endl;
    std::cout << "  - Datagrams: Unreliable typing indicators" << std::endl;
    std::cout << "  - Multiple rooms with broadcast" << std::endl;
    std::cout << "  - User presence tracking" << std::endl;
    std::cout << std::endl;

    ChatServer chat_server;

    // Simulate incoming WebTransport connections for demo
    std::cout << "In a real deployment, this would:" << std::endl;
    std::cout << "  1. Listen on UDP port for QUIC connections" << std::endl;
    std::cout << "  2. Handle HTTP/3 CONNECT with :protocol=webtransport" << std::endl;
    std::cout << "  3. Create WebTransportConnection for each session" << std::endl;
    std::cout << std::endl;

    // Demo: Create simulated connections
    std::cout << "--- Simulating Chat Session ---" << std::endl;

    // Create two simulated users
    auto make_demo_connection = [](uint64_t seed) {
        ConnectionID local_cid, peer_cid;
        local_cid.length = 8;
        peer_cid.length = 8;
        std::memcpy(local_cid.data, &seed, 8);
        seed ^= 0xFFFFFFFFFFFFFFFF;
        std::memcpy(peer_cid.data, &seed, 8);

        auto quic = std::make_unique<QUICConnection>(true, local_cid, peer_cid);
        quic->initialize();

        auto wt = std::make_unique<WebTransportConnection>(std::move(quic));
        wt->initialize();
        wt->accept();

        return wt;
    };

    // Add demo users
    auto wt1 = make_demo_connection(0x1234);
    auto wt2 = make_demo_connection(0x5678);

    chat_server.on_connection(std::move(wt1));
    chat_server.on_connection(std::move(wt2));

    std::cout << std::endl;
    std::cout << "Demo complete. In production, run this with a QUIC/HTTP/3 stack." << std::endl;
    std::cout << std::endl;

    // Protocol message format documentation
    std::cout << "--- Protocol Message Formats ---" << std::endl;
    std::cout << std::endl;
    std::cout << "Stream Messages (reliable):" << std::endl;
    std::cout << "  CHAT_MESSAGE:    [0x01][username_len][username][message]" << std::endl;
    std::cout << "  JOIN_ROOM:       [0x02][room_name]" << std::endl;
    std::cout << "  LEAVE_ROOM:      [0x03]" << std::endl;
    std::cout << "  USER_LIST:       [0x04][users...]" << std::endl;
    std::cout << "  SYSTEM_MESSAGE:  [0x05][message]" << std::endl;
    std::cout << std::endl;
    std::cout << "Datagram Messages (unreliable):" << std::endl;
    std::cout << "  TYPING_START:    [0x10][username_len][username]" << std::endl;
    std::cout << "  TYPING_STOP:     [0x11][username_len][username]" << std::endl;
    std::cout << "  PRESENCE_PING:   [0x12]" << std::endl;
    std::cout << std::endl;

    return 0;
}
