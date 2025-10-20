/**
 * 1MRC Server using libuv (mature, optimized event loop)
 * 
 * libuv is what uvloop/Node.js/many production systems use.
 * It's already vendored in FasterAPI!
 * 
 * Expected performance: 100K-500K req/s (same as uvloop)
 */

#include <uv.h>
#include <iostream>
#include <string>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <cstring>

/**
 * Thread-safe event store
 */
class EventStore {
public:
    EventStore() : total_requests_(0), sum_(0.0) {}
    
    void add_event(const std::string& user_id, double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        total_requests_++;
        sum_ += value;
        users_[user_id] = true;
    }
    
    void get_stats(uint64_t& total, uint64_t& unique, double& sum, double& avg) {
        std::lock_guard<std::mutex> lock(mutex_);
        total = total_requests_;
        unique = users_.size();
        sum = sum_;
        avg = (total > 0) ? (sum / total) : 0.0;
    }
    
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        total_requests_ = 0;
        sum_ = 0.0;
        users_.clear();
    }
    
private:
    uint64_t total_requests_;
    double sum_;
    std::unordered_map<std::string, bool> users_;
    std::mutex mutex_;
};

/**
 * JSON parser
 */
struct EventData {
    std::string user_id;
    double value;
    bool valid;
};

EventData parse_event(const std::string& json) {
    EventData data;
    data.valid = false;
    data.value = 0.0;
    
    size_t user_id_pos = json.find("\"userId\"");
    if (user_id_pos == std::string::npos) return data;
    
    size_t user_value_start = json.find(':', user_id_pos);
    if (user_value_start == std::string::npos) return data;
    
    size_t quote_start = json.find('"', user_value_start);
    if (quote_start == std::string::npos) return data;
    
    size_t quote_end = json.find('"', quote_start + 1);
    if (quote_end == std::string::npos) return data;
    
    data.user_id = json.substr(quote_start + 1, quote_end - quote_start - 1);
    
    size_t value_pos = json.find("\"value\"");
    if (value_pos == std::string::npos) return data;
    
    size_t value_start = json.find(':', value_pos);
    if (value_start == std::string::npos) return data;
    
    value_start++;
    while (value_start < json.length() && 
           (json[value_start] == ' ' || json[value_start] == '\t')) {
        value_start++;
    }
    
    size_t value_end = value_start;
    while (value_end < json.length() && 
           (std::isdigit(json[value_end]) || json[value_end] == '.' || json[value_end] == '-')) {
        value_end++;
    }
    
    if (value_end > value_start) {
        data.value = std::stod(json.substr(value_start, value_end - value_start));
        data.valid = true;
    }
    
    return data;
}

/**
 * Connection state
 */
struct Connection {
    uv_tcp_t handle;
    char buffer[8192];
    size_t bytes_read{0};
    EventStore* store;
};

// Global store
EventStore g_store;

/**
 * Write callback
 */
void on_write(uv_write_t* req, int status) {
    // Free write request
    delete[] reinterpret_cast<char*>(req->data);
    delete req;
}

/**
 * Send HTTP response
 */
void send_response(uv_stream_t* client, int status, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << " OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << body.length() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << body;
    
    std::string resp_str = response.str();
    
    // Allocate buffer for write
    char* resp_buf = new char[resp_str.length()];
    memcpy(resp_buf, resp_str.c_str(), resp_str.length());
    
    uv_buf_t buf = uv_buf_init(resp_buf, resp_str.length());
    
    uv_write_t* req = new uv_write_t();
    req->data = resp_buf;
    
    uv_write(req, client, &buf, 1, on_write);
    uv_close((uv_handle_t*)client, [](uv_handle_t* handle) {
        Connection* conn = reinterpret_cast<Connection*>(handle->data);
        delete conn;
    });
}

/**
 * Read callback
 */
void on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) {
    Connection* conn = reinterpret_cast<Connection*>(client->data);
    
    if (nread < 0) {
        // Connection closed
        uv_close((uv_handle_t*)client, [](uv_handle_t* handle) {
            Connection* conn = reinterpret_cast<Connection*>(handle->data);
            delete conn;
        });
        return;
    }
    
    if (nread == 0) return;
    
    // Copy to connection buffer
    if (conn->bytes_read + nread < sizeof(conn->buffer)) {
        memcpy(conn->buffer + conn->bytes_read, buf->base, nread);
        conn->bytes_read += nread;
    }
    
    // Check for complete request
    std::string request(conn->buffer, conn->bytes_read);
    size_t header_end = request.find("\r\n\r\n");
    
    if (header_end != std::string::npos) {
        // Stop reading
        uv_read_stop(client);
        
        // Route request
        if (request.find("POST /event") == 0) {
            std::string body = request.substr(header_end + 4);
            auto data = parse_event(body);
            
            if (data.valid) {
                conn->store->add_event(data.user_id, data.value);
                send_response(client, 201, "{\"status\":\"ok\"}");
            } else {
                send_response(client, 400, "{\"error\":\"Invalid request\"}");
            }
        } else if (request.find("GET /stats") == 0) {
            uint64_t total, unique;
            double sum, avg;
            conn->store->get_stats(total, unique, sum, avg);
            
            std::ostringstream json;
            json << std::fixed << std::setprecision(2);
            json << "{\"totalRequests\":" << total
                 << ",\"uniqueUsers\":" << unique
                 << ",\"sum\":" << sum
                 << ",\"avg\":" << avg << "}";
            
            send_response(client, 200, json.str());
        } else if (request.find("GET /health") == 0) {
            send_response(client, 200, "{\"status\":\"healthy\"}");
        } else if (request.find("POST /reset") == 0) {
            conn->store->reset();
            send_response(client, 200, "{\"status\":\"reset\"}");
        } else {
            send_response(client, 404, "{\"error\":\"Not Found\"}");
        }
    }
}

/**
 * Allocation callback
 */
void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

/**
 * Connection callback
 */
void on_connection(uv_stream_t* server, int status) {
    if (status < 0) {
        std::cerr << "Connection error" << std::endl;
        return;
    }
    
    // Create new connection
    Connection* conn = new Connection();
    conn->bytes_read = 0;
    conn->store = &g_store;
    
    uv_tcp_init(uv_default_loop(), &conn->handle);
    conn->handle.data = conn;
    
    if (uv_accept(server, (uv_stream_t*)&conn->handle) == 0) {
        // Start reading
        uv_read_start((uv_stream_t*)&conn->handle, on_alloc, on_read);
    } else {
        uv_close((uv_handle_t*)&conn->handle, [](uv_handle_t* handle) {
            Connection* conn = reinterpret_cast<Connection*>(handle->data);
            delete conn;
        });
    }
}

int main() {
    std::cout << "============================================================\n";
    std::cout << "FasterAPI libuv - 1MRC Server\n";
    std::cout << "============================================================\n";
    std::cout << "Using libuv " << uv_version_string() << "\n";
    std::cout << "============================================================\n";
    
    uv_loop_t* loop = uv_default_loop();
    
    // Create TCP server
    uv_tcp_t server;
    uv_tcp_init(loop, &server);
    
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", 8000, &addr);
    
    uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
    
    int r = uv_listen((uv_stream_t*)&server, 2048, on_connection);
    if (r) {
        std::cerr << "Listen error: " << uv_strerror(r) << std::endl;
        return 1;
    }
    
    std::cout << "Server listening on 0.0.0.0:8000\n";
    std::cout << "Endpoints:\n";
    std::cout << "  POST /event  - Accept event data\n";
    std::cout << "  GET  /stats  - Get aggregated statistics\n";
    std::cout << "  GET  /health - Health check\n";
    std::cout << "  POST /reset  - Reset statistics\n";
    std::cout << "\nUsing libuv's optimized event loop!\n";
    std::cout << "Expected: 100K-500K req/s (same as uvloop) 🚀\n";
    std::cout << "============================================================\n";
    
    // Run event loop
    return uv_run(loop, UV_RUN_DEFAULT);
}

