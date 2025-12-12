/**
 * HTTP/3 Server Example - Real QUIC/HTTP/3 with TLS 1.3
 *
 * Demonstrates:
 * - HTTP/3 over QUIC with native TLS 1.3
 * - QPACK header compression
 * - Self-signed certificate generation
 * - WebTransport support
 *
 * Build:
 *   cmake --build build --target http3_server
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/http3_server
 *
 * Test with curl (requires HTTP/3 support):
 *   curl --http3 -k https://localhost:8443/
 *
 * Test with Chrome:
 *   /Applications/Google\ Chrome.app/Contents/MacOS/Google\ Chrome \
 *       --origin-to-force-quic-on=localhost:8443 \
 *       --ignore-certificate-errors \
 *       https://localhost:8443/
 */

#include "../src/cpp/http/quic/http3_server.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

using namespace fasterapi;

// Simple JSON builder
class JsonResponse {
public:
    JsonResponse& object() { ss_ << "{"; first_ = true; return *this; }
    JsonResponse& end_object() { ss_ << "}"; return *this; }
    JsonResponse& array() { ss_ << "["; first_ = true; return *this; }
    JsonResponse& end_array() { ss_ << "]"; return *this; }

    JsonResponse& key(const std::string& k) {
        if (!first_) ss_ << ",";
        ss_ << "\"" << k << "\":";
        first_ = false;
        return *this;
    }

    JsonResponse& value(const std::string& v) {
        if (!first_) ss_ << ",";
        ss_ << "\"" << escape(v) << "\"";
        first_ = false;
        return *this;
    }

    JsonResponse& value(int v) {
        if (!first_) ss_ << ",";
        ss_ << v;
        first_ = false;
        return *this;
    }

    JsonResponse& value(bool v) {
        if (!first_) ss_ << ",";
        ss_ << (v ? "true" : "false");
        first_ = false;
        return *this;
    }

    std::string build() { return ss_.str(); }

private:
    std::ostringstream ss_;
    bool first_ = true;

    std::string escape(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '"') r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else r += c;
        }
        return r;
    }
};

std::string get_current_time() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

int main() {
    std::cout << "=== FasterAPI HTTP/3 Server ===" << std::endl;
    std::cout << std::endl;

    http3::HTTP3Server server;

    // Use self-signed certificate
    if (!server.configure_tls_self_signed()) {
        std::cerr << "Failed to configure TLS" << std::endl;
        return 1;
    }

    std::cout << "TLS configured with self-signed certificate" << std::endl;

    // Set request handler
    server.set_request_handler([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::vector<uint8_t>& body,
        std::function<void(int, const std::unordered_map<std::string, std::string>&,
                           const std::vector<uint8_t>&)> respond) {

        std::cout << "[HTTP/3] " << method << " " << path << std::endl;

        std::unordered_map<std::string, std::string> resp_headers;
        std::vector<uint8_t> resp_body;

        if (path == "/" || path == "/index.html") {
            // Serve HTML page
            resp_headers["content-type"] = "text/html; charset=utf-8";
            resp_headers["alt-svc"] = "h3=\":8443\"; ma=86400";

            std::string html = R"HTML(<!DOCTYPE html>
<html>
<head>
    <title>FasterAPI HTTP/3 Server</title>
    <style>
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            max-width: 800px;
            margin: 0 auto;
            padding: 40px 20px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            color: white;
        }
        .card {
            background: rgba(255,255,255,0.95);
            border-radius: 16px;
            padding: 32px;
            margin: 20px 0;
            color: #333;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
        }
        h1 { margin: 0 0 8px 0; color: #667eea; }
        .badge {
            display: inline-block;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 4px 12px;
            border-radius: 20px;
            font-size: 12px;
            margin-bottom: 20px;
        }
        .info { background: #f8f9fa; padding: 16px; border-radius: 8px; margin: 16px 0; }
        .info h3 { margin: 0 0 12px 0; color: #667eea; }
        .info-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #eee; }
        .info-row:last-child { border-bottom: none; }
        .label { color: #666; }
        .value { font-family: monospace; color: #333; }
        .success { color: #28a745; }
        pre {
            background: #1a1a2e;
            color: #4ecca3;
            padding: 16px;
            border-radius: 8px;
            overflow-x: auto;
            font-size: 13px;
        }
        .endpoints { margin-top: 24px; }
        .endpoint {
            display: flex;
            align-items: center;
            padding: 12px;
            background: #f8f9fa;
            border-radius: 8px;
            margin: 8px 0;
        }
        .method {
            font-weight: bold;
            padding: 4px 8px;
            border-radius: 4px;
            margin-right: 12px;
            font-size: 12px;
        }
        .method.get { background: #28a745; color: white; }
        .method.post { background: #007bff; color: white; }
        .path { font-family: monospace; color: #333; }
        button {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            padding: 12px 24px;
            border-radius: 8px;
            cursor: pointer;
            font-size: 14px;
            margin: 8px 8px 8px 0;
        }
        button:hover { opacity: 0.9; }
        #response { white-space: pre-wrap; }
    </style>
</head>
<body>
    <div class="card">
        <h1>FasterAPI HTTP/3</h1>
        <span class="badge">QUIC + TLS 1.3</span>

        <div class="info">
            <h3>Connection Info</h3>
            <div class="info-row">
                <span class="label">Protocol</span>
                <span class="value success" id="protocol">Detecting...</span>
            </div>
            <div class="info-row">
                <span class="label">TLS Version</span>
                <span class="value">TLS 1.3</span>
            </div>
            <div class="info-row">
                <span class="label">ALPN</span>
                <span class="value">h3</span>
            </div>
        </div>

        <div class="endpoints">
            <h3>API Endpoints</h3>
            <div class="endpoint">
                <span class="method get">GET</span>
                <span class="path">/api/info</span>
            </div>
            <div class="endpoint">
                <span class="method get">GET</span>
                <span class="path">/api/time</span>
            </div>
            <div class="endpoint">
                <span class="method post">POST</span>
                <span class="path">/api/echo</span>
            </div>
        </div>

        <div style="margin-top: 24px;">
            <button onclick="fetchInfo()">Get Server Info</button>
            <button onclick="fetchTime()">Get Server Time</button>
            <button onclick="testEcho()">Test Echo</button>
        </div>

        <pre id="response">Click a button to test the API...</pre>
    </div>

    <script>
        // Check if using HTTP/3
        if (window.chrome && window.chrome.loadTimes) {
            var lt = window.chrome.loadTimes();
            document.getElementById('protocol').textContent =
                lt.connectionInfo || 'HTTP/3 (QUIC)';
        } else if (navigator.connection && navigator.connection.protocol) {
            document.getElementById('protocol').textContent =
                navigator.connection.protocol;
        } else {
            document.getElementById('protocol').textContent = 'HTTP/3 (QUIC)';
        }

        async function fetchInfo() {
            try {
                const resp = await fetch('/api/info');
                const data = await resp.json();
                document.getElementById('response').textContent =
                    JSON.stringify(data, null, 2);
            } catch (e) {
                document.getElementById('response').textContent = 'Error: ' + e.message;
            }
        }

        async function fetchTime() {
            try {
                const resp = await fetch('/api/time');
                const data = await resp.json();
                document.getElementById('response').textContent =
                    JSON.stringify(data, null, 2);
            } catch (e) {
                document.getElementById('response').textContent = 'Error: ' + e.message;
            }
        }

        async function testEcho() {
            try {
                const resp = await fetch('/api/echo', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ message: 'Hello HTTP/3!', timestamp: Date.now() })
                });
                const data = await resp.json();
                document.getElementById('response').textContent =
                    JSON.stringify(data, null, 2);
            } catch (e) {
                document.getElementById('response').textContent = 'Error: ' + e.message;
            }
        }
    </script>
</body>
</html>)HTML";

            resp_body.assign(html.begin(), html.end());
            respond(200, resp_headers, resp_body);

        } else if (path == "/api/info") {
            // Server info API
            resp_headers["content-type"] = "application/json";

            JsonResponse json;
            json.object()
                .key("server").value("FasterAPI HTTP/3")
                .key("version").value("1.0.0")
                .key("protocol").value("HTTP/3")
                .key("transport").value("QUIC v1")
                .key("tls_version").value("TLS 1.3")
                .key("features").array()
                    .value("HTTP/3")
                    .value("QPACK")
                    .value("WebTransport")
                    .value("DATAGRAM")
                .end_array()
            .end_object();

            std::string body = json.build();
            resp_body.assign(body.begin(), body.end());
            respond(200, resp_headers, resp_body);

        } else if (path == "/api/time") {
            // Time API
            resp_headers["content-type"] = "application/json";

            JsonResponse json;
            json.object()
                .key("time").value(get_current_time())
                .key("timezone").value("UTC")
            .end_object();

            std::string body = json.build();
            resp_body.assign(body.begin(), body.end());
            respond(200, resp_headers, resp_body);

        } else if (path == "/api/echo" && method == "POST") {
            // Echo API
            resp_headers["content-type"] = "application/json";

            JsonResponse json;
            json.object()
                .key("echo").value(std::string(body.begin(), body.end()))
                .key("received_at").value(get_current_time())
                .key("size").value(static_cast<int>(body.size()))
            .end_object();

            std::string resp = json.build();
            resp_body.assign(resp.begin(), resp.end());
            respond(200, resp_headers, resp_body);

        } else {
            // 404 Not Found
            resp_headers["content-type"] = "application/json";

            JsonResponse json;
            json.object()
                .key("error").value("Not Found")
                .key("path").value(path)
            .end_object();

            std::string body = json.build();
            resp_body.assign(body.begin(), body.end());
            respond(404, resp_headers, resp_body);
        }
    });

    // Set WebTransport handler
    server.set_webtransport_handler([](
        uint64_t session_id,
        std::function<void(const uint8_t*, size_t)> send_datagram,
        std::function<void()> close_session) {

        std::cout << "[WebTransport] New session: " << session_id << std::endl;

        // Echo back any datagrams received
        // (In a real app, you'd store these callbacks and use them later)
    });

    // Start server
    uint16_t port = 8443;
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }

    std::cout << std::endl;
    std::cout << "HTTP/3 server running on https://localhost:" << port << std::endl;
    std::cout << std::endl;
    std::cout << "To test with curl (requires HTTP/3 support):" << std::endl;
    std::cout << "  curl --http3 -k https://localhost:" << port << "/" << std::endl;
    std::cout << std::endl;
    std::cout << "To test with Chrome:" << std::endl;
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
