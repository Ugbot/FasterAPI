/**
 * 1 Million Request Challenge - Async I/O Version
 * 
 * Uses FasterAPI's async_io framework with kqueue/epoll/io_uring/IOCP.
 * Expected performance: 500K-2M req/s (50-200x faster than thread-per-connection!)
 */

#include "src/cpp/core/async_io.h"
#include <iostream>
#include <string>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

using namespace fasterapi::core;

/**
 * Thread-safe event store (same as before)
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
 * Server context (shared across all connections)
 */
struct ServerContext {
    async_io* io;
    EventStore* store;
    int listen_fd;
};

/**
 * Connection state
 */
struct Connection {
    int fd;
    char buffer[8192];
    size_t bytes_read{0};
    ServerContext* ctx;
};

/**
 * Simple JSON parser (same as before)
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
 * HTTP response helper
 */
void send_response(ServerContext* ctx, int fd, int status, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << " OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << body.length() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << body;
    
    std::string resp_str = response.str();
    char* resp_buf = new char[resp_str.length()];
    memcpy(resp_buf, resp_str.c_str(), resp_str.length());
    
    ctx->io->write_async(fd, resp_buf, resp_str.length(), 
        [fd, resp_buf](const io_event& ev) {
            delete[] resp_buf;
            close(fd);  // Close after write
        });
}

/**
 * Handle HTTP request
 */
void handle_request(Connection* conn, const std::string& request) {
    ServerContext* ctx = conn->ctx;
    
    // Simple routing
    if (request.find("POST /event") == 0) {
        // Find body
        size_t body_start = request.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            std::string body = request.substr(body_start + 4);
            auto data = parse_event(body);
            
            if (data.valid) {
                ctx->store->add_event(data.user_id, data.value);
                send_response(ctx, conn->fd, 201, "{\"status\":\"ok\"}");
            } else {
                send_response(ctx, conn->fd, 400, "{\"error\":\"Invalid request\"}");
            }
        } else {
            send_response(ctx, conn->fd, 400, "{\"error\":\"No body\"}");
        }
    } else if (request.find("GET /stats") == 0) {
        uint64_t total, unique;
        double sum, avg;
        ctx->store->get_stats(total, unique, sum, avg);
        
        std::ostringstream json;
        json << std::fixed << std::setprecision(2);
        json << "{\"totalRequests\":" << total
             << ",\"uniqueUsers\":" << unique
             << ",\"sum\":" << sum
             << ",\"avg\":" << avg << "}";
        
        send_response(ctx, conn->fd, 200, json.str());
    } else if (request.find("GET /health") == 0) {
        send_response(ctx, conn->fd, 200, "{\"status\":\"healthy\"}");
    } else if (request.find("POST /reset") == 0) {
        ctx->store->reset();
        send_response(ctx, conn->fd, 200, "{\"status\":\"reset\"}");
    } else {
        send_response(ctx, conn->fd, 404, "{\"error\":\"Not Found\"}");
    }
    
    delete conn;
}

/**
 * Read handler - called when data is available
 */
void on_read(const io_event& ev) {
    Connection* conn = static_cast<Connection*>(ev.user_data);
    
    if (ev.result <= 0) {
        // Connection closed or error
        close(conn->fd);
        delete conn;
        return;
    }
    
    conn->bytes_read += ev.result;
    
    // Check if we have complete request (ends with \r\n\r\n)
    std::string request(conn->buffer, conn->bytes_read);
    if (request.find("\r\n\r\n") != std::string::npos) {
        // Complete request - handle it
        handle_request(conn, request);
    } else if (conn->bytes_read < sizeof(conn->buffer)) {
        // Read more data
        conn->ctx->io->read_async(
            conn->fd,
            conn->buffer + conn->bytes_read,
            sizeof(conn->buffer) - conn->bytes_read,
            on_read,
            conn
        );
    } else {
        // Buffer full - send error
        send_response(conn->ctx, conn->fd, 413, "{\"error\":\"Request too large\"}");
    }
}

// Forward declaration
void on_accept(const io_event& ev);

/**
 * Accept handler - called when new connection arrives
 */
void on_accept(const io_event& ev) {
    ServerContext* ctx = static_cast<ServerContext*>(ev.user_data);
    
    if (ev.result < 0) {
        std::cerr << "Accept failed: " << ev.result << std::endl;
        // Continue accepting despite error
        ctx->io->accept_async(ctx->listen_fd, on_accept, ctx);
        return;
    }
    
    int client_fd = ev.result;
    
    // Create connection state
    Connection* conn = new Connection();
    conn->fd = client_fd;
    conn->ctx = ctx;
    conn->bytes_read = 0;
    
    // Start reading from client
    ctx->io->read_async(
        client_fd,
        conn->buffer,
        sizeof(conn->buffer),
        on_read,
        conn
    );
    
    // Accept next connection (continue loop!)
    ctx->io->accept_async(ctx->listen_fd, on_accept, ctx);
}

int main() {
    std::cout << "============================================================\n";
    std::cout << "FasterAPI Async I/O - 1MRC Server\n";
    std::cout << "============================================================\n";
    
    // Create async I/O engine (auto-detects kqueue/epoll/etc)
    auto io = async_io::create();
    if (!io) {
        std::cerr << "Failed to create async I/O engine" << std::endl;
        return 1;
    }
    
    std::cout << "Async I/O backend: " << io->backend_name() << "\n";
    std::cout << "============================================================\n";
    
    // Create event store
    EventStore store;
    
    // Create listen socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8000);
    
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind socket" << std::endl;
        close(listen_fd);
        return 1;
    }
    
    if (listen(listen_fd, 2048) < 0) {
        std::cerr << "Failed to listen" << std::endl;
        close(listen_fd);
        return 1;
    }
    
    std::cout << "Server listening on 0.0.0.0:8000\n";
    std::cout << "Endpoints:\n";
    std::cout << "  POST /event  - Accept event data\n";
    std::cout << "  GET  /stats  - Get aggregated statistics\n";
    std::cout << "  GET  /health - Health check\n";
    std::cout << "  POST /reset  - Reset statistics\n";
    std::cout << "\nExpected performance: 500K-2M req/s! 🚀\n";
    std::cout << "============================================================\n";
    
    // Create context for callbacks (needs to live for entire program)
    ServerContext* ctx = new ServerContext{io.get(), &store, listen_fd};
    
    // Start accepting connections
    io->accept_async(listen_fd, on_accept, ctx);
    
    // Run event loop
    io->run();
    
    close(listen_fd);
    delete ctx;
    return 0;
}

