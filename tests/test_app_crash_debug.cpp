/**
 * Debug test to reproduce App destructor crash with HTTP/3 routes
 */

#include "../src/cpp/http/app.h"
#include <iostream>

using namespace fasterapi;

__attribute__((destructor))
static void cleanup_hook() {
    std::cerr << "[DESTRUCTOR HOOK] Global cleanup running" << std::endl;
}

int main() {
    std::cout << "Creating App with HTTP/3 enabled..." << std::endl;

    App::Config config;
    config.enable_http3 = false;  // TEST: Disable HTTP/3
    config.enable_docs = false;
    config.http3_port = 9443;

    {
        App app(config);
        std::cout << "App created successfully on stack" << std::endl;

        std::cout << "Registering FIVE routes..." << std::endl;
        app.get("/", [](Request& req, Response& res) {
            res.json({{"message", "root"}});
        });
        app.post("/users", [](Request& req, Response& res) {
            res.json({{"action", "create"}});
        });
        app.get("/users/{id}", [](Request& req, Response& res) {
            auto id = req.path_param("id");
            res.json({{"id", id}});
        });
        app.put("/users/{id}", [](Request& req, Response& res) {
            auto id = req.path_param("id");
            res.json({{"action", "update"}, {"id", id}});
        });
        app.del("/users/{id}", [](Request& req, Response& res) {
            auto id = req.path_param("id");
            res.json({{"action", "delete"}, {"id", id}});
        });

        std::cout << "5 routes registered" << std::endl;
        std::cerr << "[TEST] About to exit scope, app at address: " << (void*)&app << std::endl;
    }
    std::cerr << "[TEST] Exited scope successfully!" << std::endl;

    std::cout << "App destroyed successfully!" << std::endl;
    return 0;
}
