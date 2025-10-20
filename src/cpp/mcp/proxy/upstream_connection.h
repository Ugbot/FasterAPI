#pragma once

#include "proxy_core.h"
#include "../transports/transport.h"
#include <memory>
#include <string>

namespace fasterapi {
namespace mcp {
namespace proxy {

/**
 * STDIO-based upstream connection (subprocess)
 */
class StdioUpstreamConnection : public UpstreamConnection {
public:
    explicit StdioUpstreamConnection(const UpstreamConfig& config);
    ~StdioUpstreamConnection() override;

    bool connect() override;
    void disconnect() override;
    std::optional<std::string> send_request(const std::string& request, uint32_t timeout_ms) override;
    bool is_healthy() const override;
    std::string get_name() const override;

private:
    UpstreamConfig config_;
    std::shared_ptr<Transport> transport_;
    bool connected_ = false;
};

/**
 * HTTP-based upstream connection
 */
class HttpUpstreamConnection : public UpstreamConnection {
public:
    explicit HttpUpstreamConnection(const UpstreamConfig& config);
    ~HttpUpstreamConnection() override;

    bool connect() override;
    void disconnect() override;
    std::optional<std::string> send_request(const std::string& request, uint32_t timeout_ms) override;
    bool is_healthy() const override;
    std::string get_name() const override;

private:
    UpstreamConfig config_;
    bool connected_ = false;

    // HTTP client state
    // TODO: Add HTTP client implementation
};

/**
 * WebSocket-based upstream connection
 */
class WebSocketUpstreamConnection : public UpstreamConnection {
public:
    explicit WebSocketUpstreamConnection(const UpstreamConfig& config);
    ~WebSocketUpstreamConnection() override;

    bool connect() override;
    void disconnect() override;
    std::optional<std::string> send_request(const std::string& request, uint32_t timeout_ms) override;
    bool is_healthy() const override;
    std::string get_name() const override;

private:
    UpstreamConfig config_;
    bool connected_ = false;

    // WebSocket client state
    // TODO: Add WebSocket client implementation
};

/**
 * Factory for creating upstream connections
 */
class UpstreamConnectionFactory {
public:
    static std::shared_ptr<UpstreamConnection> create(const UpstreamConfig& config);
};

} // namespace proxy
} // namespace mcp
} // namespace fasterapi
