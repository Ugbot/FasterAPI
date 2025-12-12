/**
 * Drogon Benchmark Server
 * 
 * Implements the same TechEmpower endpoints as pure_cpp_server
 * for fair comparison.
 */

#include <drogon/drogon.h>
#include <random>
#include <array>
#include <charconv>

using namespace drogon;

// Simulated World table (same as FasterAPI)
static std::array<int, 10001> g_world_table;

// Thread-local RNG for performance
thread_local std::mt19937 t_rng{std::random_device{}()};
thread_local std::uniform_int_distribution<int> t_id_dist{1, 10000};
thread_local std::uniform_int_distribution<int> t_rand_dist{1, 10000};

// Fast integer to string
inline void append_int(std::string& out, int val) {
    char buf[16];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
    out.append(buf, ptr - buf);
}

// Parse queries parameter (clamp 1-500)
inline int parse_queries(const std::string& param) {
    if (param.empty()) return 1;
    int val = 0;
    auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), val);
    if (ec != std::errc()) return 1;
    if (val < 1) return 1;
    if (val > 500) return 500;
    return val;
}

int main() {
    // Initialize World table
    std::mt19937 init_rng{42};
    std::uniform_int_distribution<int> init_dist{1, 10000};
    for (int i = 1; i <= 10000; i++) {
        g_world_table[i] = init_dist(init_rng);
    }

    // JSON test
    app().registerHandler(
        "/json",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"message":"Hello, World!"})");
            callback(resp);
        },
        {Get});

    // Plaintext test
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

    // DB test - single query
    app().registerHandler(
        "/db",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            int id = t_id_dist(t_rng);
            int randomNumber = g_world_table[id];
            
            std::string json;
            json.reserve(48);
            json = R"({"id":)";
            append_int(json, id);
            json += R"(,"randomNumber":)";
            append_int(json, randomNumber);
            json += "}";
            
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(std::move(json));
            callback(resp);
        },
        {Get});

    // Queries test - multiple queries
    app().registerHandler(
        "/queries",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            auto queries_param = req->getParameter("queries");
            int queries = parse_queries(queries_param);
            
            std::string json;
            json.reserve(32 * queries + 2);
            json = "[";
            
            for (int i = 0; i < queries; i++) {
                if (i > 0) json += ",";
                int id = t_id_dist(t_rng);
                int randomNumber = g_world_table[id];
                
                json += R"({"id":)";
                append_int(json, id);
                json += R"(,"randomNumber":)";
                append_int(json, randomNumber);
                json += "}";
            }
            json += "]";
            
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(std::move(json));
            callback(resp);
        },
        {Get});

    // Updates test - multiple updates
    app().registerHandler(
        "/updates",
        [](const HttpRequestPtr& req,
           std::function<void(const HttpResponsePtr&)>&& callback) {
            auto queries_param = req->getParameter("queries");
            int queries = parse_queries(queries_param);
            
            std::string json;
            json.reserve(32 * queries + 2);
            json = "[";
            
            for (int i = 0; i < queries; i++) {
                if (i > 0) json += ",";
                int id = t_id_dist(t_rng);
                int newRandomNumber = t_rand_dist(t_rng);
                g_world_table[id] = newRandomNumber;
                
                json += R"({"id":)";
                append_int(json, id);
                json += R"(,"randomNumber":)";
                append_int(json, newRandomNumber);
                json += "}";
            }
            json += "]";
            
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(std::move(json));
            callback(resp);
        },
        {Get});

    std::cout << "Drogon benchmark server starting on http://127.0.0.1:8081" << std::endl;
    
    app().addListener("127.0.0.1", 8081);
    app().setThreadNum(0);  // Auto-detect threads
    app().disableSession();
    app().run();
    
    return 0;
}
