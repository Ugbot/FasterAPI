/**
 * Minimal test to debug server issues
 */
#include "src/cpp/http/server.h"
#include "src/cpp/http/request.h"
#include "src/cpp/http/response.h"
#include <iostream>

int main() {
    // Create server config
    HttpServer::Config config;
    config.port = 8000;
    config.host = "0.0.0.0";
    config.enable_h1 = true;
    config.enable_h2 = false;

    // Create server
    HttpServer server(config);

    // Add single route
    std::cout << "Adding route GET /\n";
    int result = server.add_route("GET", "/",
        [](HttpRequest* req, HttpResponse* res, const fasterapi::http::RouteParams& params) {
            std::cout << "Handler called!\n";
            res->status(HttpResponse::Status::OK)
               .content_type("text/plain")
               .text("Hello, World!")
               .send();
        }
    );

    if (result != 0) {
        std::cerr << "Failed to add route: " << result << "\n";
        return 1;
    }

    // Start server
    std::cout << "Starting server on port 8000...\n";
    result = server.start();

    if (result != 0) {
        std::cerr << "Failed to start server: " << result << "\n";
        return 1;
    }

    return 0;
}
