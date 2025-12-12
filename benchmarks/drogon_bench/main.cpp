/**
 * Drogon Benchmark Server
 * 
 * Simple HTTP server for benchmarking against FasterAPI.
 * Implements the same endpoints as pure_cpp_server for fair comparison.
 */

#include <drogon/drogon.h>
#include <json/json.h>

using namespace drogon;

int main(int argc, char* argv[]) {
    int port = 8081;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    // Configure server
    app()
        .setLogLevel(trantor::Logger::kWarn)
        .addListener("127.0.0.1", port)
        .setThreadNum(10)  // Match FasterAPI's 10 workers
        .enableRunAsDaemon();

    // Health endpoint (JSON)
    app().registerHandler(
        "/health",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            Json::Value json;
            json["status"] = "ok";
            json["server"] = "drogon";
            auto resp = HttpResponse::newHttpJsonResponse(json);
            callback(resp);
        },
        {Get});

    // Root endpoint (HTML)
    app().registerHandler(
        "/",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_TEXT_HTML);
            resp->setBody(R"(
<!DOCTYPE html>
<html>
<head><title>Drogon Benchmark</title></head>
<body>
    <h1>Drogon Benchmark Server</h1>
    <p>High-performance C++ HTTP server</p>
</body>
</html>
)");
            callback(resp);
        },
        {Get});

    // JSON endpoint
    app().registerHandler(
        "/json",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            Json::Value json;
            json["message"] = "Hello, World!";
            auto resp = HttpResponse::newHttpJsonResponse(json);
            callback(resp);
        },
        {Get});

    // Plaintext endpoint
    app().registerHandler(
        "/plaintext",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_TEXT_PLAIN);
            resp->setBody("Hello, World!");
            callback(resp);
        },
        {Get});

    std::cout << "Drogon benchmark server starting on http://127.0.0.1:" << port << std::endl;
    std::cout << "Workers: 10" << std::endl;
    
    app().run();
    
    return 0;
}
