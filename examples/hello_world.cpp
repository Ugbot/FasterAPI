/**
 * @file hello_world.cpp
 * @brief Minimal "Hello World" example using FasterAPI C++ API
 *
 * This is the simplest possible FasterAPI application.
 *
 * Compile and run:
 * @code
 * mkdir build && cd build
 * cmake ..
 * make hello_world
 * ./hello_world
 * @endcode
 *
 * Then visit http://localhost:8000/
 */

#include "../src/cpp/http/app.h"
#include <iostream>

using namespace fasterapi;

int main() {
    // Create application
    auto app = App();

    // Define a route
    app.get("/", [](Request& req, Response& res) {
        std::cerr << "[hello_world] Lambda called!" << std::endl;
        std::cerr << "[hello_world] About to call res.json()" << std::endl;
        res.json({
            {"message", "Hello, World!"},
            {"from", "FasterAPI C++"}
        });
        std::cerr << "[hello_world] res.json() returned" << std::endl;
    });

    // Run server
    std::cout << "Server starting on http://localhost:8000\n";
    return app.run("0.0.0.0", 8000);
}
