/**
 * REST API Example - Complete CRUD API with error handling
 *
 * Demonstrates:
 * - Full CRUD operations (Create, Read, Update, Delete)
 * - Path parameters and query parameters
 * - Proper HTTP status codes
 * - JSON request/response handling
 * - Error handling with appropriate responses
 * - In-memory data store pattern
 *
 * Build:
 *   cmake --build build --target rest_api_example
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/rest_api_example
 *
 * Test:
 *   curl http://localhost:8080/api/products
 *   curl -X POST http://localhost:8080/api/products -H "Content-Type: application/json" \
 *        -d '{"name":"Widget","price":9.99,"quantity":100}'
 */

#include "../src/cpp/http/app.h"
#include <iostream>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <sstream>
#include <iomanip>

using namespace fasterapi;

// Simple Product model
struct Product {
    int id;
    std::string name;
    double price;
    int quantity;

    std::string to_json() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << R"({"id":)" << id
           << R"(,"name":")" << name << R"(")"
           << R"(,"price":)" << price
           << R"(,"quantity":)" << quantity << "}";
        return ss.str();
    }
};

// Thread-safe in-memory store
class ProductStore {
public:
    ProductStore() : next_id_(1) {
        // Seed with some initial data
        create("Widget", 9.99, 100);
        create("Gadget", 19.99, 50);
        create("Gizmo", 29.99, 25);
    }

    Product create(const std::string& name, double price, int quantity) {
        std::lock_guard<std::mutex> lock(mutex_);
        Product p{next_id_++, name, price, quantity};
        products_[p.id] = p;
        return p;
    }

    std::optional<Product> get(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = products_.find(id);
        if (it != products_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<Product> list(int offset = 0, int limit = 10) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Product> result;
        int i = 0;
        for (const auto& [id, product] : products_) {
            if (i >= offset && result.size() < static_cast<size_t>(limit)) {
                result.push_back(product);
            }
            i++;
        }
        return result;
    }

    bool update(int id, const std::string& name, double price, int quantity) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = products_.find(id);
        if (it != products_.end()) {
            it->second.name = name;
            it->second.price = price;
            it->second.quantity = quantity;
            return true;
        }
        return false;
    }

    bool remove(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return products_.erase(id) > 0;
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return products_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<int, Product> products_;
    std::atomic<int> next_id_;
};

// Simple JSON parser helpers (for demo - use simdjson in production)
std::string extract_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    auto end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

double extract_number(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0.0;
    pos += search.length();
    return std::stod(json.substr(pos));
}

int main() {
    std::cout << "=== REST API Example ===" << std::endl;

    ProductStore store;

    App::Config config;
    config.pure_cpp_mode = true;
    config.title = "Product API";
    config.version = "1.0.0";

    App app(config);

    // List all products
    // GET /api/products?offset=0&limit=10
    app.get("/api/products", [&store](Request& req, Response& res) {
        auto offset_str = req.query_param_optional("offset").value_or("0");
        auto limit_str = req.query_param_optional("limit").value_or("10");

        int offset = std::stoi(offset_str);
        int limit = std::stoi(limit_str);

        auto products = store.list(offset, limit);

        std::ostringstream json;
        json << R"({"products":[)";
        for (size_t i = 0; i < products.size(); i++) {
            if (i > 0) json << ",";
            json << products[i].to_json();
        }
        json << R"(],"total":)" << store.count()
             << R"(,"offset":)" << offset
             << R"(,"limit":)" << limit << "}";

        res.json(json.str());
    });

    // Get single product
    // GET /api/products/{id}
    app.get("/api/products/{id}", [&store](Request& req, Response& res) {
        std::string id_str = req.path_param("id");
        int id = std::stoi(id_str);

        auto product = store.get(id);
        if (product) {
            res.json(product->to_json());
        } else {
            res.not_found().json(R"({"error":"Product not found","id":)" + id_str + "}");
        }
    });

    // Create product
    // POST /api/products
    app.post("/api/products", [&store](Request& req, Response& res) {
        std::string body = req.body();

        std::string name = extract_string(body, "name");
        double price = extract_number(body, "price");
        int quantity = static_cast<int>(extract_number(body, "quantity"));

        if (name.empty()) {
            res.bad_request().json(R"({"error":"Name is required"})");
            return;
        }
        if (price <= 0) {
            res.bad_request().json(R"({"error":"Price must be positive"})");
            return;
        }

        auto product = store.create(name, price, quantity);
        res.created().json(product.to_json());
    });

    // Update product
    // PUT /api/products/{id}
    app.put("/api/products/{id}", [&store](Request& req, Response& res) {
        std::string id_str = req.path_param("id");
        int id = std::stoi(id_str);
        std::string body = req.body();

        std::string name = extract_string(body, "name");
        double price = extract_number(body, "price");
        int quantity = static_cast<int>(extract_number(body, "quantity"));

        if (store.update(id, name, price, quantity)) {
            auto product = store.get(id);
            res.json(product->to_json());
        } else {
            res.not_found().json(R"({"error":"Product not found"})");
        }
    });

    // Delete product
    // DELETE /api/products/{id}
    app.del("/api/products/{id}", [&store](Request& req, Response& res) {
        std::string id_str = req.path_param("id");
        int id = std::stoi(id_str);

        if (store.remove(id)) {
            res.no_content().send("");
        } else {
            res.not_found().json(R"({"error":"Product not found"})");
        }
    });

    // Search products (bonus endpoint)
    // GET /api/products/search?q=widget
    app.get("/api/products/search", [&store](Request& req, Response& res) {
        auto query = req.query_param_optional("q").value_or("");

        auto products = store.list(0, 100);
        std::ostringstream json;
        json << R"({"results":[)";
        bool first = true;
        for (const auto& p : products) {
            // Simple case-insensitive substring search
            std::string name_lower = p.name;
            std::string query_lower = query;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

            if (name_lower.find(query_lower) != std::string::npos) {
                if (!first) json << ",";
                json << p.to_json();
                first = false;
            }
        }
        json << R"(],"query":")" << query << "\"}";

        res.json(json.str());
    });

    std::cout << "Starting on http://localhost:8080" << std::endl;
    std::cout << "Endpoints:" << std::endl;
    std::cout << "  GET    /api/products           - List all products" << std::endl;
    std::cout << "  GET    /api/products/{id}      - Get product by ID" << std::endl;
    std::cout << "  POST   /api/products           - Create product" << std::endl;
    std::cout << "  PUT    /api/products/{id}      - Update product" << std::endl;
    std::cout << "  DELETE /api/products/{id}      - Delete product" << std::endl;
    std::cout << "  GET    /api/products/search?q= - Search products" << std::endl;

    return app.run_unified("0.0.0.0", 8080);
}
