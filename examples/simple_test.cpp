/**
 * Minimal test without docs routes
 */

#include "../src/cpp/http/app.h"
#include <iostream>

using namespace fasterapi;

int main() {
    // Create application with docs disabled
    App::Config config;
    config.enable_docs = false;
    auto app = App(config);

    // Define a route
    app.get("/", [](Request& req, Response& res) {
        std::cerr << "[SIMPLE] Handler called!" << std::endl;
        std::cerr.flush();
        res.send("Hello!");
        std::cerr << "[SIMPLE] Handler finished!" << std::endl;
        std::cerr.flush();
    });

    std::cout << "Simple test starting on http://localhost:8000\n";
    return app.run("0.0.0.0", 8000);
}
