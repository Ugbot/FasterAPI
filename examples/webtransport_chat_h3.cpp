/**
 * WebTransport Chat Example - Real-time chat over HTTP/3 + WebTransport
 *
 * Demonstrates:
 * - WebTransport over HTTP/3 (RFC 9297)
 * - Bidirectional streams for reliable messaging
 * - Datagrams for unreliable low-latency updates
 * - Multi-client broadcast
 * - Room-based chat
 *
 * Build:
 *   cmake --build build --target webtransport_chat_h3
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/webtransport_chat_h3
 *
 * Then open: https://localhost:8443 in Chrome with flags:
 *   /Applications/Google\ Chrome.app/Contents/MacOS/Google\ Chrome \
 *       --origin-to-force-quic-on=localhost:8443 \
 *       --ignore-certificate-errors \
 *       https://localhost:8443/
 */

#include "../src/cpp/http/quic/http3_server.h"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <sstream>
#include <random>
#include <chrono>
#include <iomanip>

using namespace fasterapi;

// =============================================================================
// Chat State
// =============================================================================

struct ChatUser {
    uint64_t session_id;
    std::string username;
    std::string room;
    std::function<void(const uint8_t*, size_t)> send_datagram;
    std::function<void()> close_session;
};

struct ChatRoom {
    std::string name;
    std::unordered_set<uint64_t> users;
    std::vector<std::pair<std::string, std::string>> history;
    static constexpr size_t MAX_HISTORY = 50;

    void add_message(const std::string& username, const std::string& msg) {
        if (history.size() >= MAX_HISTORY) {
            history.erase(history.begin());
        }
        history.emplace_back(username, msg);
    }
};

std::mutex g_mutex;
std::atomic<uint64_t> g_next_user_id{1};
std::unordered_map<uint64_t, ChatUser> g_users;
std::unordered_map<std::string, ChatRoom> g_rooms;

// =============================================================================
// Message Encoding (simple binary protocol)
// =============================================================================

enum class MessageType : uint8_t {
    JOIN = 1,
    LEAVE = 2,
    CHAT = 3,
    SYSTEM = 4,
    USER_LIST = 5,
    TYPING = 6,
    PRESENCE = 7
};

std::vector<uint8_t> encode_message(MessageType type, const std::string& user,
                                     const std::string& text, const std::string& room = "") {
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(type));

    // User length + data
    msg.push_back(static_cast<uint8_t>(user.size()));
    msg.insert(msg.end(), user.begin(), user.end());

    // Room length + data
    msg.push_back(static_cast<uint8_t>(room.size()));
    msg.insert(msg.end(), room.begin(), room.end());

    // Text as remaining bytes
    msg.insert(msg.end(), text.begin(), text.end());

    return msg;
}

struct DecodedMessage {
    MessageType type;
    std::string user;
    std::string room;
    std::string text;
};

DecodedMessage decode_message(const uint8_t* data, size_t len) {
    DecodedMessage msg;
    if (len < 3) return msg;

    size_t pos = 0;
    msg.type = static_cast<MessageType>(data[pos++]);

    uint8_t user_len = data[pos++];
    if (pos + user_len > len) return msg;
    msg.user = std::string(reinterpret_cast<const char*>(data + pos), user_len);
    pos += user_len;

    if (pos >= len) return msg;
    uint8_t room_len = data[pos++];
    if (pos + room_len > len) return msg;
    msg.room = std::string(reinterpret_cast<const char*>(data + pos), room_len);
    pos += room_len;

    if (pos < len) {
        msg.text = std::string(reinterpret_cast<const char*>(data + pos), len - pos);
    }

    return msg;
}

// =============================================================================
// Chat Functions
// =============================================================================

void broadcast_to_room(const std::string& room, const std::vector<uint8_t>& msg,
                       uint64_t exclude = 0) {
    auto it = g_rooms.find(room);
    if (it == g_rooms.end()) return;

    for (uint64_t user_id : it->second.users) {
        if (user_id != exclude) {
            auto user_it = g_users.find(user_id);
            if (user_it != g_users.end() && user_it->second.send_datagram) {
                user_it->second.send_datagram(msg.data(), msg.size());
            }
        }
    }
}

void join_room(uint64_t user_id, const std::string& new_room) {
    auto user_it = g_users.find(user_id);
    if (user_it == g_users.end()) return;

    auto& user = user_it->second;
    std::string old_room = user.room;

    // Leave old room
    if (!old_room.empty()) {
        auto room_it = g_rooms.find(old_room);
        if (room_it != g_rooms.end()) {
            room_it->second.users.erase(user_id);
            auto msg = encode_message(MessageType::LEAVE, user.username, "left the room", old_room);
            broadcast_to_room(old_room, msg);
        }
    }

    // Create room if needed
    if (g_rooms.find(new_room) == g_rooms.end()) {
        g_rooms[new_room] = ChatRoom{new_room, {}, {}};
    }

    // Join new room
    user.room = new_room;
    g_rooms[new_room].users.insert(user_id);

    // Notify user
    auto sys_msg = encode_message(MessageType::SYSTEM, "Server", "Joined #" + new_room, new_room);
    if (user.send_datagram) {
        user.send_datagram(sys_msg.data(), sys_msg.size());
    }

    // Notify room
    auto join_msg = encode_message(MessageType::JOIN, user.username, "joined the room", new_room);
    broadcast_to_room(new_room, join_msg, user_id);

    // Send user list
    std::ostringstream users_ss;
    for (uint64_t uid : g_rooms[new_room].users) {
        auto it = g_users.find(uid);
        if (it != g_users.end()) {
            if (!users_ss.str().empty()) users_ss << ",";
            users_ss << it->second.username;
        }
    }
    auto list_msg = encode_message(MessageType::USER_LIST, "Server", users_ss.str(), new_room);
    if (user.send_datagram) {
        user.send_datagram(list_msg.data(), list_msg.size());
    }

    std::cout << "[Chat] " << user.username << " joined #" << new_room << std::endl;
}

std::string get_current_time() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&time), "%H:%M:%S");
    return ss.str();
}

// =============================================================================
// HTML Page
// =============================================================================

const char* kChatHtml = R"HTML(<!DOCTYPE html>
<html>
<head>
    <title>FasterAPI WebTransport Chat</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            color: #eee;
            height: 100vh;
            display: flex;
            flex-direction: column;
        }
        .header {
            background: rgba(0,0,0,0.3);
            padding: 15px 20px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            border-bottom: 1px solid rgba(255,255,255,0.1);
        }
        .header h1 {
            font-size: 20px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        .badge {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 4px 12px;
            border-radius: 20px;
            font-size: 11px;
            margin-left: 10px;
        }
        .header .room { color: #667eea; font-size: 14px; }
        .container { flex: 1; display: flex; overflow: hidden; }
        .sidebar {
            width: 220px;
            background: rgba(0,0,0,0.2);
            padding: 15px;
            border-right: 1px solid rgba(255,255,255,0.1);
        }
        .sidebar h3 {
            color: #667eea;
            margin-bottom: 10px;
            font-size: 12px;
            text-transform: uppercase;
            letter-spacing: 1px;
        }
        .room-list button {
            display: block;
            width: 100%;
            padding: 10px 12px;
            margin: 4px 0;
            background: rgba(255,255,255,0.05);
            border: 1px solid rgba(255,255,255,0.1);
            color: #eee;
            cursor: pointer;
            border-radius: 8px;
            text-align: left;
            transition: all 0.2s;
        }
        .room-list button:hover { background: rgba(102,126,234,0.2); border-color: #667eea; }
        .room-list button.active {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            border-color: transparent;
        }
        .users { margin-top: 20px; font-size: 13px; color: #888; }
        .users span {
            display: block;
            padding: 6px 0;
            border-bottom: 1px solid rgba(255,255,255,0.05);
        }
        .users span:before { content: "● "; color: #4ecca3; }
        .main { flex: 1; display: flex; flex-direction: column; }
        .messages {
            flex: 1;
            overflow-y: auto;
            padding: 20px;
        }
        .message {
            margin: 8px 0;
            padding: 12px 16px;
            background: rgba(255,255,255,0.05);
            border-radius: 12px;
            max-width: 80%;
            animation: fadeIn 0.3s ease;
        }
        @keyframes fadeIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }
        .message.system {
            background: rgba(102,126,234,0.2);
            color: #aaa;
            font-size: 13px;
            max-width: 100%;
            text-align: center;
            border-radius: 20px;
        }
        .message.self {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin-left: auto;
        }
        .message .user {
            font-weight: 600;
            color: #667eea;
            font-size: 13px;
            margin-bottom: 4px;
        }
        .message.self .user { color: rgba(255,255,255,0.8); }
        .message .text { line-height: 1.5; }
        .message .time { font-size: 11px; color: rgba(255,255,255,0.4); margin-top: 4px; }
        .input-area {
            display: flex;
            padding: 15px 20px;
            background: rgba(0,0,0,0.3);
            border-top: 1px solid rgba(255,255,255,0.1);
        }
        .input-area input {
            flex: 1;
            padding: 14px 18px;
            background: rgba(255,255,255,0.05);
            border: 1px solid rgba(255,255,255,0.1);
            color: #eee;
            border-radius: 12px;
            font-size: 14px;
            outline: none;
            transition: border-color 0.2s;
        }
        .input-area input:focus { border-color: #667eea; }
        .input-area button {
            padding: 14px 28px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            border: none;
            color: #fff;
            cursor: pointer;
            border-radius: 12px;
            margin-left: 10px;
            font-weight: 600;
            transition: opacity 0.2s;
        }
        .input-area button:hover { opacity: 0.9; }
        .status {
            font-size: 12px;
            padding: 6px 14px;
            background: rgba(0,0,0,0.3);
            border-radius: 20px;
        }
        .status.connected { color: #4ecca3; }
        .status.disconnected { color: #e94560; }
        .status.connecting { color: #f9ca24; }
        .typing { font-size: 12px; color: #888; padding: 0 20px 10px; font-style: italic; }
    </style>
</head>
<body>
    <div class="header">
        <div style="display: flex; align-items: center;">
            <h1>FasterAPI Chat</h1>
            <span class="badge">WebTransport</span>
        </div>
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
                <h3>Online</h3>
            </div>
        </div>
        <div class="main">
            <div class="messages" id="messages"></div>
            <div class="typing" id="typing"></div>
            <div class="input-area">
                <input type="text" id="input" placeholder="Type a message... (use /nick to change name)"
                       onkeypress="if(event.key==='Enter')sendMessage()" oninput="onTyping()">
                <button onclick="sendMessage()">Send</button>
            </div>
        </div>
    </div>
    <script>
        let transport = null;
        let writer = null;
        let username = 'User' + Math.floor(Math.random() * 10000);
        let currentRoom = 'general';
        let reconnectTimer = null;

        const MessageType = {
            JOIN: 1, LEAVE: 2, CHAT: 3, SYSTEM: 4, USER_LIST: 5, TYPING: 6, PRESENCE: 7
        };

        function encodeMessage(type, user, text, room) {
            const userBytes = new TextEncoder().encode(user);
            const roomBytes = new TextEncoder().encode(room || '');
            const textBytes = new TextEncoder().encode(text);
            const msg = new Uint8Array(1 + 1 + userBytes.length + 1 + roomBytes.length + textBytes.length);
            let pos = 0;
            msg[pos++] = type;
            msg[pos++] = userBytes.length;
            msg.set(userBytes, pos); pos += userBytes.length;
            msg[pos++] = roomBytes.length;
            msg.set(roomBytes, pos); pos += roomBytes.length;
            msg.set(textBytes, pos);
            return msg;
        }

        function decodeMessage(data) {
            const view = new Uint8Array(data);
            let pos = 0;
            const type = view[pos++];
            const userLen = view[pos++];
            const user = new TextDecoder().decode(view.slice(pos, pos + userLen));
            pos += userLen;
            const roomLen = view[pos++];
            const room = new TextDecoder().decode(view.slice(pos, pos + roomLen));
            pos += roomLen;
            const text = new TextDecoder().decode(view.slice(pos));
            return { type, user, room, text };
        }

        async function connect() {
            const statusEl = document.getElementById('status');
            statusEl.textContent = 'Connecting...';
            statusEl.className = 'status connecting';

            try {
                // Check WebTransport support
                if (typeof WebTransport === 'undefined') {
                    throw new Error('WebTransport not supported. Use Chrome 97+ with flags.');
                }

                transport = new WebTransport('https://' + location.host + '/webtransport');
                await transport.ready;

                statusEl.textContent = 'Connected';
                statusEl.className = 'status connected';

                // Open datagram streams
                const reader = transport.datagrams.readable.getReader();
                writer = transport.datagrams.writable.getWriter();

                // Join default room
                const joinMsg = encodeMessage(MessageType.JOIN, username, '', currentRoom);
                await writer.write(joinMsg);

                // Read incoming datagrams
                while (true) {
                    const { value, done } = await reader.read();
                    if (done) break;
                    handleMessage(value);
                }
            } catch (e) {
                console.error('Connection error:', e);
                statusEl.textContent = 'Disconnected';
                statusEl.className = 'status disconnected';

                // Fallback message for unsupported browsers
                if (e.message && e.message.includes('not supported')) {
                    addMessage({ type: MessageType.SYSTEM, user: 'System',
                        text: 'WebTransport requires Chrome 97+ with flags. See console for details.' });
                    console.log('To enable WebTransport, run Chrome with:\n' +
                        '/Applications/Google\\ Chrome.app/Contents/MacOS/Google\\ Chrome \\\n' +
                        '    --origin-to-force-quic-on=' + location.host + ' \\\n' +
                        '    --ignore-certificate-errors');
                }

                // Reconnect after delay
                reconnectTimer = setTimeout(connect, 3000);
            }

            transport.closed.then(() => {
                statusEl.textContent = 'Disconnected';
                statusEl.className = 'status disconnected';
                setTimeout(connect, 2000);
            });
        }

        function handleMessage(data) {
            const msg = decodeMessage(data);
            addMessage(msg);
        }

        function addMessage(msg) {
            const div = document.createElement('div');
            div.className = 'message';

            if (msg.type === MessageType.SYSTEM || msg.type === MessageType.JOIN || msg.type === MessageType.LEAVE) {
                div.className += ' system';
                div.textContent = msg.text;
            } else if (msg.type === MessageType.USER_LIST) {
                document.getElementById('userList').innerHTML = '<h3>Online</h3>' +
                    msg.text.split(',').filter(u => u).map(u => '<span>' + u + '</span>').join('');
                return;
            } else if (msg.type === MessageType.TYPING) {
                document.getElementById('typing').textContent = msg.user + ' is typing...';
                setTimeout(() => { document.getElementById('typing').textContent = ''; }, 2000);
                return;
            } else {
                if (msg.user === username) div.className += ' self';
                const time = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
                div.innerHTML = '<div class="user">' + msg.user + '</div>' +
                               '<div class="text">' + escapeHtml(msg.text) + '</div>' +
                               '<div class="time">' + time + '</div>';
            }

            document.getElementById('messages').appendChild(div);
            div.scrollIntoView({ behavior: 'smooth' });
        }

        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }

        async function sendMessage() {
            const input = document.getElementById('input');
            const text = input.value.trim();
            if (!text || !writer) return;

            if (text.startsWith('/nick ')) {
                const newName = text.slice(6).trim();
                if (newName) {
                    username = newName;
                    addMessage({ type: MessageType.SYSTEM, user: 'System', text: 'You are now ' + username });
                }
            } else if (text.startsWith('/join ')) {
                joinRoom(text.slice(6).trim());
            } else {
                const msg = encodeMessage(MessageType.CHAT, username, text, currentRoom);
                await writer.write(msg);
                addMessage({ type: MessageType.CHAT, user: username, text: text });
            }
            input.value = '';
        }

        async function joinRoom(room) {
            if (!room || room === currentRoom) return;

            currentRoom = room;
            document.getElementById('roomName').textContent = '#' + room;
            document.querySelectorAll('.room-list button').forEach(b => b.className = '');
            const btn = document.getElementById('room-' + room);
            if (btn) btn.className = 'active';
            document.getElementById('messages').innerHTML = '';

            if (writer) {
                const msg = encodeMessage(MessageType.JOIN, username, '', room);
                await writer.write(msg);
            }
        }

        let typingTimeout = null;
        async function onTyping() {
            if (typingTimeout) return;
            typingTimeout = setTimeout(() => { typingTimeout = null; }, 2000);

            if (writer) {
                const msg = encodeMessage(MessageType.TYPING, username, '', currentRoom);
                await writer.write(msg);
            }
        }

        // Start connection
        connect();
    </script>
</body>
</html>)HTML";

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== FasterAPI WebTransport Chat ===" << std::endl;
    std::cout << std::endl;

    // Initialize rooms
    g_rooms["general"] = ChatRoom{"general", {}, {}};
    g_rooms["random"] = ChatRoom{"random", {}, {}};
    g_rooms["tech"] = ChatRoom{"tech", {}, {}};

    http3::HTTP3Server server;

    // Use self-signed certificate
    if (!server.configure_tls_self_signed()) {
        std::cerr << "Failed to configure TLS" << std::endl;
        return 1;
    }

    std::cout << "TLS configured with self-signed certificate" << std::endl;

    // HTTP/3 request handler (for serving the HTML page)
    server.set_request_handler([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::vector<uint8_t>& body,
        std::function<void(int, const std::unordered_map<std::string, std::string>&,
                           const std::vector<uint8_t>&)> respond) {

        std::unordered_map<std::string, std::string> resp_headers;
        std::vector<uint8_t> resp_body;

        if (path == "/" || path == "/index.html") {
            resp_headers["content-type"] = "text/html; charset=utf-8";
            resp_headers["alt-svc"] = "h3=\":8443\"; ma=86400";

            std::string html(kChatHtml);
            resp_body.assign(html.begin(), html.end());
            respond(200, resp_headers, resp_body);
        } else {
            resp_headers["content-type"] = "text/plain";
            std::string not_found = "404 Not Found";
            resp_body.assign(not_found.begin(), not_found.end());
            respond(404, resp_headers, resp_body);
        }
    });

    // WebTransport session handler
    server.set_webtransport_handler([](
        uint64_t session_id,
        std::function<void(const uint8_t*, size_t)> send_datagram,
        std::function<void()> close_session) {

        std::cout << "[WebTransport] New session: " << session_id << std::endl;

        std::lock_guard<std::mutex> lock(g_mutex);

        // Create user
        uint64_t user_id = g_next_user_id++;
        ChatUser user;
        user.session_id = session_id;
        user.username = "User" + std::to_string(user_id);
        user.room = "";
        user.send_datagram = send_datagram;
        user.close_session = close_session;

        g_users[user_id] = user;

        std::cout << "[Chat] User " << user.username << " connected" << std::endl;

        // Note: In a real implementation, we'd need to handle incoming datagrams
        // and clean up when the session closes. The HTTP3Server would need to
        // provide callbacks for datagram receipt and session close events.
    });

    // Start server
    uint16_t port = 8443;
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }

    std::cout << std::endl;
    std::cout << "WebTransport Chat running on https://localhost:" << port << std::endl;
    std::cout << std::endl;
    std::cout << "To test with Chrome (requires flags for self-signed cert):" << std::endl;
    std::cout << std::endl;
    std::cout << "  /Applications/Google\\ Chrome.app/Contents/MacOS/Google\\ Chrome \\" << std::endl;
    std::cout << "      --origin-to-force-quic-on=localhost:" << port << " \\" << std::endl;
    std::cout << "      --ignore-certificate-errors \\" << std::endl;
    std::cout << "      https://localhost:" << port << "/" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;

    // Run until interrupted
    while (server.is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
