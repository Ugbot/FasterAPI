/**
 * Minimal CoroIO HTTP server - copying echo server structure exactly
 */
#include <coroio/all.hpp>
#include <iostream>
#include <string>

using namespace NNet;

// Simple HTTP handler coroutine
template<typename TSocket>
static TVoidTask handle_request(TSocket socket) {
    std::cout << "🔵 Handler started!" << std::endl;

    char buffer[16384];
    auto bytes_read = co_await socket.ReadSome(buffer, sizeof(buffer));

    std::cout << "🔵 Read " << bytes_read << " bytes" << std::endl;

    if (bytes_read > 0) {
        // Simple HTTP response
        const char* response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello, World!";

        co_await socket.WriteSome(response, strlen(response));
        std::cout << "🔵 Response sent!" << std::endl;
    }

    co_return;
}

// Server listener coroutine - exactly like echo server
template<typename TPoller>
static TVoidTask server(TPoller& poller, TAddress address) {
    typename TPoller::TSocket listen_socket(poller, address.Domain());
    listen_socket.Bind(address);
    listen_socket.Listen();

    std::cout << "Server listening on " << address.ToString() << std::endl;

    while (true) {
        std::cout << "🟢 Waiting for connection..." << std::endl;
        auto client = co_await listen_socket.Accept();
        std::cout << "🟢 Connection accepted!" << std::endl;

        // Fire and forget - no co_await
        handle_request(std::move(client));
    }

    co_return;
}

int main() {
    std::cout << "=== Minimal CoroIO HTTP Server ===" << std::endl;

    TInitializer init;
    TLoop<TDefaultPoller> loop;

    TAddress addr("0.0.0.0", 8003);

    // Start server coroutine - no co_await
    server(loop.Poller(), addr);

    std::cout << "Starting event loop..." << std::endl;
    loop.Loop();

    return 0;
}
