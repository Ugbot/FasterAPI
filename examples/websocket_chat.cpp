/**
 * WebSocket Chat Example - Real-time chat using WebSocket
 *
 * Demonstrates:
 * - WebSocket session management
 * - Multi-client broadcast
 * - Room-based chat
 * - User presence tracking
 * - HTML/JavaScript client
 *
 * Build:
 *   cmake --build build --target websocket_chat
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/websocket_chat
 *
 * Then open: http://localhost:8080 in Chrome
 */

#include "../src/cpp/http/app.h"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <sstream>
#include <random>

using namespace fasterapi;

// =============================================================================
// Chat Room
// =============================================================================

struct ChatRoom {
    std::string name;
    std::unordered_set<http::WebSocketConnection*> clients;
    std::vector<std::pair<std::string, std::string>> history;
    static constexpr size_t MAX_HISTORY = 50;

    void add_message(const std::string& username, const std::string& msg) {
        if (history.size() >= MAX_HISTORY) {
            history.erase(history.begin());
        }
        history.emplace_back(username, msg);
    }
};

// =============================================================================
// Chat Server State
// =============================================================================

struct UserInfo {
    std::string username;
    std::string room;
    http::WebSocketConnection* ws;
};

std::mutex g_mutex;
std::atomic<uint64_t> g_next_id{1};
std::unordered_map<http::WebSocketConnection*, UserInfo> g_users;
std::unordered_map<std::string, ChatRoom> g_rooms;

// =============================================================================
// JSON Helpers
// =============================================================================

std::string json_escape(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

std::string make_json(const std::string& type, const std::string& user, const std::string& text) {
    std::ostringstream ss;
    ss << "{\"type\":\"" << type << "\""
       << ",\"user\":\"" << json_escape(user) << "\""
       << ",\"text\":\"" << json_escape(text) << "\"}";
    return ss.str();
}

std::string extract_json_field(const std::string& json, const std::string& field) {
    std::string search = "\"" + field + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    auto end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// =============================================================================
// Chat Functions
// =============================================================================

void broadcast_to_room(const std::string& room, const std::string& msg, http::WebSocketConnection* exclude = nullptr) {
    auto it = g_rooms.find(room);
    if (it == g_rooms.end()) return;

    for (auto* ws : it->second.clients) {
        if (ws != exclude) {
            ws->send_text(msg);
        }
    }
}

void join_room(http::WebSocketConnection* ws, const std::string& new_room) {
    auto user_it = g_users.find(ws);
    if (user_it == g_users.end()) return;

    auto& user = user_it->second;
    std::string old_room = user.room;

    // Leave old room
    if (!old_room.empty()) {
        auto room_it = g_rooms.find(old_room);
        if (room_it != g_rooms.end()) {
            room_it->second.clients.erase(ws);
            broadcast_to_room(old_room, make_json("leave", user.username, "left the room"));
        }
    }

    // Create room if needed
    if (g_rooms.find(new_room) == g_rooms.end()) {
        g_rooms[new_room] = ChatRoom{new_room, {}, {}};
    }

    // Join new room
    user.room = new_room;
    g_rooms[new_room].clients.insert(ws);

    // Notify
    ws->send_text(make_json("system", "Server", "Joined #" + new_room));
    broadcast_to_room(new_room, make_json("join", user.username, "joined the room"), ws);

    // Send user list
    std::ostringstream users_ss;
    for (auto* client : g_rooms[new_room].clients) {
        auto it = g_users.find(client);
        if (it != g_users.end()) {
            if (!users_ss.str().empty()) users_ss << ", ";
            users_ss << it->second.username;
        }
    }
    ws->send_text(make_json("users", "Server", users_ss.str()));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== FasterAPI WebSocket Chat ===" << std::endl;

    // Initialize rooms
    g_rooms["general"] = ChatRoom{"general", {}, {}};
    g_rooms["random"] = ChatRoom{"random", {}, {}};
    g_rooms["tech"] = ChatRoom{"tech", {}, {}};

    App::Config config;
    config.pure_cpp_mode = true;
    App app(config);

    // Serve HTML page
    app.get("/", [](Request& req, Response& res) {
        res.html(R"HTMLPAGE(<!DOCTYPE html>
<html>
<head>
    <title>FasterAPI WebSocket Chat</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; background: #1a1a2e; color: #eee; height: 100vh; display: flex; flex-direction: column; }
        .header { background: #16213e; padding: 15px 20px; display: flex; justify-content: space-between; align-items: center; }
        .header h1 { font-size: 20px; color: #e94560; }
        .header .room { color: #0f4c75; font-size: 14px; }
        .container { flex: 1; display: flex; overflow: hidden; }
        .sidebar { width: 200px; background: #16213e; padding: 15px; border-right: 1px solid #333; }
        .sidebar h3 { color: #0f4c75; margin-bottom: 10px; font-size: 12px; text-transform: uppercase; }
        .room-list button { display: block; width: 100%; padding: 8px; margin: 4px 0; background: #1a1a2e; border: 1px solid #333; color: #eee; cursor: pointer; border-radius: 4px; text-align: left; }
        .room-list button:hover { background: #0f4c75; }
        .room-list button.active { background: #e94560; }
        .users { margin-top: 20px; font-size: 13px; color: #888; }
        .users span { display: block; padding: 4px 0; }
        .main { flex: 1; display: flex; flex-direction: column; }
        .messages { flex: 1; overflow-y: auto; padding: 20px; }
        .message { margin: 8px 0; padding: 10px 14px; background: #16213e; border-radius: 8px; max-width: 80%; }
        .message.system { background: #0f4c75; color: #aaa; font-size: 13px; max-width: 100%; text-align: center; }
        .message.self { background: #e94560; margin-left: auto; }
        .message .user { font-weight: bold; color: #0f4c75; font-size: 13px; }
        .message.self .user { color: #fff; }
        .message .text { margin-top: 4px; line-height: 1.4; }
        .input-area { display: flex; padding: 15px; background: #16213e; border-top: 1px solid #333; }
        .input-area input { flex: 1; padding: 12px; background: #1a1a2e; border: 1px solid #333; color: #eee; border-radius: 6px; font-size: 14px; }
        .input-area button { padding: 12px 24px; background: #e94560; border: none; color: #fff; cursor: pointer; border-radius: 6px; margin-left: 10px; font-weight: bold; }
        .input-area button:hover { background: #d13a54; }
        .status { font-size: 12px; padding: 5px 10px; background: #333; border-radius: 4px; }
        .status.connected { color: #4ecca3; }
        .status.disconnected { color: #e94560; }
    </style>
</head>
<body>
    <div class="header">
        <h1>FasterAPI Chat</h1>
        <span class="room" id="roomName">#general</span>
        <span class="status disconnected" id="status">Disconnected</span>
    </div>
    <div class="container">
        <div class="sidebar">
            <h3>Rooms</h3>
            <div class="room-list">
                <button onclick="joinRoom('general')" id="room-general" class="active">#general</button>
                <button onclick="joinRoom('random')" id="room-random">#random</button>
                <button onclick="joinRoom('tech')" id="room-tech">#tech</button>
            </div>
            <div class="users" id="userList">
                <h3>Users</h3>
            </div>
        </div>
        <div class="main">
            <div class="messages" id="messages"></div>
            <div class="input-area">
                <input type="text" id="input" placeholder="Type a message... (/nick, /join)" onkeypress="if(event.key==='Enter')sendMessage()">
                <button onclick="sendMessage()">Send</button>
            </div>
        </div>
    </div>
    <script>
        var ws;
        var username = 'User' + Math.floor(Math.random() * 1000);
        var currentRoom = 'general';

        function connect() {
            ws = new WebSocket('ws://' + location.host + '/ws');

            ws.onopen = function() {
                document.getElementById('status').textContent = 'Connected';
                document.getElementById('status').className = 'status connected';
                ws.send(JSON.stringify({type: 'join', room: 'general', user: username}));
            };

            ws.onmessage = function(e) {
                var msg = JSON.parse(e.data);
                addMessage(msg);
            };

            ws.onclose = function() {
                document.getElementById('status').textContent = 'Disconnected';
                document.getElementById('status').className = 'status disconnected';
                setTimeout(connect, 2000);
            };
        }

        function addMessage(msg) {
            var div = document.createElement('div');
            div.className = 'message';

            if (msg.type === 'system' || msg.type === 'join' || msg.type === 'leave') {
                div.className += ' system';
                div.textContent = msg.text;
            } else if (msg.type === 'users') {
                document.getElementById('userList').innerHTML = '<h3>Users</h3>' +
                    msg.text.split(', ').map(function(u) { return '<span>' + u + '</span>'; }).join('');
                return;
            } else {
                if (msg.user === username) div.className += ' self';
                div.innerHTML = '<div class="user">' + msg.user + '</div><div class="text">' + msg.text + '</div>';
            }

            document.getElementById('messages').appendChild(div);
            div.scrollIntoView({behavior: 'smooth'});
        }

        function sendMessage() {
            var input = document.getElementById('input');
            var text = input.value.trim();
            if (!text) return;

            if (text.indexOf('/nick ') === 0) {
                username = text.slice(6);
                addMessage({type: 'system', text: 'You are now ' + username});
            } else if (text.indexOf('/join ') === 0) {
                joinRoom(text.slice(6));
            } else {
                ws.send(JSON.stringify({type: 'chat', user: username, text: text}));
            }
            input.value = '';
        }

        function joinRoom(room) {
            currentRoom = room;
            document.getElementById('roomName').textContent = '#' + room;
            var buttons = document.querySelectorAll('.room-list button');
            for (var i = 0; i < buttons.length; i++) buttons[i].className = '';
            var btn = document.getElementById('room-' + room);
            if (btn) btn.className = 'active';
            document.getElementById('messages').innerHTML = '';
            ws.send(JSON.stringify({type: 'join', room: room, user: username}));
        }

        connect();
    </script>
</body>
</html>
)HTMLPAGE");
    });

    // WebSocket endpoint
    app.websocket("/ws", [](http::WebSocketConnection& ws) {
        std::cout << "[Chat] New connection" << std::endl;

        ws.on_text_message = [&ws](const std::string& msg) {
            std::lock_guard<std::mutex> lock(g_mutex);

            std::string type = extract_json_field(msg, "type");
            std::string user = extract_json_field(msg, "user");
            std::string text = extract_json_field(msg, "text");
            std::string room = extract_json_field(msg, "room");

            if (type == "join") {
                auto& info = g_users[&ws];
                info.username = user;
                info.ws = &ws;
                join_room(&ws, room);
                std::cout << "[Chat] " << user << " joined #" << room << std::endl;
            } else if (type == "chat") {
                auto it = g_users.find(&ws);
                if (it != g_users.end()) {
                    auto& info = it->second;
                    info.username = user;

                    auto room_it = g_rooms.find(info.room);
                    if (room_it != g_rooms.end()) {
                        room_it->second.add_message(user, text);
                    }

                    broadcast_to_room(info.room, make_json("chat", user, text));
                    std::cout << "[#" << info.room << "] " << user << ": " << text << std::endl;
                }
            }
        };

        ws.on_close = [&ws](uint16_t code, const char* reason) {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_users.find(&ws);
            if (it != g_users.end()) {
                std::string room = it->second.room;
                std::string user = it->second.username;

                auto room_it = g_rooms.find(room);
                if (room_it != g_rooms.end()) {
                    room_it->second.clients.erase(&ws);
                    broadcast_to_room(room, make_json("leave", user, "left the chat"));
                }

                g_users.erase(it);
                std::cout << "[Chat] " << user << " disconnected" << std::endl;
            }
        };
    });

    std::cout << std::endl;
    std::cout << "Open http://localhost:8080 in Chrome" << std::endl;
    std::cout << std::endl;

    return app.run_unified("0.0.0.0", 8080);
}
