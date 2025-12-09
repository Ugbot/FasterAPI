/**
 * JSON API Example - JSON request/response with validation
 *
 * Demonstrates:
 * - JSON request parsing with simdjson
 * - Request validation with detailed errors
 * - Type-safe response building
 * - Error response formatting
 * - Complex nested JSON structures
 *
 * Build:
 *   cmake --build build --target json_api_example
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/json_api_example
 *
 * Test:
 *   curl -X POST http://localhost:8080/api/order \
 *        -H "Content-Type: application/json" \
 *        -d '{"customer":"John","items":[{"id":1,"qty":2}]}'
 */

#include "../src/cpp/http/app.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <optional>
#include <regex>

using namespace fasterapi;

// Validation result
struct ValidationResult {
    bool valid = true;
    std::vector<std::string> errors;

    void add_error(const std::string& field, const std::string& message) {
        valid = false;
        errors.push_back(field + ": " + message);
    }

    std::string to_json() const {
        std::ostringstream ss;
        ss << R"({"valid":)" << (valid ? "true" : "false");
        if (!errors.empty()) {
            ss << R"(,"errors":[)";
            for (size_t i = 0; i < errors.size(); i++) {
                if (i > 0) ss << ",";
                ss << "\"" << errors[i] << "\"";
            }
            ss << "]";
        }
        ss << "}";
        return ss.str();
    }
};

// JSON Builder for response construction
class JsonBuilder {
public:
    JsonBuilder& object() { ss_ << "{"; need_comma_ = false; return *this; }
    JsonBuilder& end_object() { ss_ << "}"; need_comma_ = true; return *this; }
    JsonBuilder& array() { ss_ << "["; need_comma_ = false; return *this; }
    JsonBuilder& end_array() { ss_ << "]"; need_comma_ = true; return *this; }

    JsonBuilder& key(const std::string& k) {
        maybe_comma();
        ss_ << "\"" << k << "\":";
        need_comma_ = false;
        return *this;
    }

    JsonBuilder& value(const std::string& v) {
        maybe_comma();
        ss_ << "\"" << escape(v) << "\"";
        need_comma_ = true;
        return *this;
    }

    JsonBuilder& value(int v) {
        maybe_comma();
        ss_ << v;
        need_comma_ = true;
        return *this;
    }

    JsonBuilder& value(double v) {
        maybe_comma();
        ss_ << std::fixed << std::setprecision(2) << v;
        need_comma_ = true;
        return *this;
    }

    JsonBuilder& value(bool v) {
        maybe_comma();
        ss_ << (v ? "true" : "false");
        need_comma_ = true;
        return *this;
    }

    JsonBuilder& null_value() {
        maybe_comma();
        ss_ << "null";
        need_comma_ = true;
        return *this;
    }

    std::string build() const { return ss_.str(); }

private:
    std::ostringstream ss_;
    bool need_comma_ = false;

    void maybe_comma() {
        if (need_comma_) ss_ << ",";
    }

    std::string escape(const std::string& s) const {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c;
            }
        }
        return result;
    }
};

// Simple JSON parser helpers
std::string extract_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();

    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (json[pos] != '"') return "";
    pos++;
    auto end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

int extract_int(const std::string& json, const std::string& key, int default_val = 0) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return default_val;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    // Check if it's a valid number
    if (pos >= json.size() || (!isdigit(json[pos]) && json[pos] != '-')) return default_val;

    char* end = nullptr;
    long val = std::strtol(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) return default_val;
    return static_cast<int>(val);
}

double extract_double(const std::string& json, const std::string& key, double default_val = 0.0) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return default_val;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    // Check if it's a valid number
    if (pos >= json.size() || (!isdigit(json[pos]) && json[pos] != '-' && json[pos] != '.')) return default_val;

    char* end = nullptr;
    double val = std::strtod(json.c_str() + pos, &end);
    if (end == json.c_str() + pos) return default_val;
    return val;
}

bool extract_bool(const std::string& json, const std::string& key, bool default_val = false) {
    std::string search = "\"" + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return default_val;
    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return json.substr(pos, 4) == "true";
}

// Email validation
bool is_valid_email(const std::string& email) {
    static std::regex email_regex(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
    return std::regex_match(email, email_regex);
}

int main() {
    std::cout << "=== JSON API Example ===" << std::endl;

    App::Config config;
    config.pure_cpp_mode = true;
    App app(config);

    // User registration with validation
    app.post("/api/register", [](Request& req, Response& res) {
        std::string body = req.body();
        ValidationResult validation;

        // Extract and validate fields
        std::string username = extract_string(body, "username");
        std::string email = extract_string(body, "email");
        std::string password = extract_string(body, "password");
        int age = extract_int(body, "age", -1);

        // Validate username
        if (username.empty()) {
            validation.add_error("username", "required");
        } else if (username.length() < 3) {
            validation.add_error("username", "must be at least 3 characters");
        } else if (username.length() > 50) {
            validation.add_error("username", "must be at most 50 characters");
        }

        // Validate email
        if (email.empty()) {
            validation.add_error("email", "required");
        } else if (!is_valid_email(email)) {
            validation.add_error("email", "invalid format");
        }

        // Validate password
        if (password.empty()) {
            validation.add_error("password", "required");
        } else if (password.length() < 8) {
            validation.add_error("password", "must be at least 8 characters");
        }

        // Validate age
        if (age < 0) {
            validation.add_error("age", "required and must be a number");
        } else if (age < 13) {
            validation.add_error("age", "must be at least 13");
        } else if (age > 120) {
            validation.add_error("age", "must be at most 120");
        }

        if (!validation.valid) {
            res.bad_request().json(validation.to_json());
            return;
        }

        // Build success response using JsonBuilder
        JsonBuilder builder;
        builder.object()
            .key("success").value(true)
            .key("user").object()
                .key("id").value(12345)
                .key("username").value(username)
                .key("email").value(email)
                .key("age").value(age)
                .key("created_at").value("2024-01-01T00:00:00Z")
            .end_object()
            .key("message").value("Registration successful")
        .end_object();

        res.created().json(builder.build());
    });

    // Order creation with nested validation
    app.post("/api/order", [](Request& req, Response& res) {
        std::string body = req.body();
        ValidationResult validation;

        std::string customer = extract_string(body, "customer");
        std::string shipping_address = extract_string(body, "shipping_address");
        bool express_shipping = extract_bool(body, "express_shipping", false);

        if (customer.empty()) {
            validation.add_error("customer", "required");
        }

        // Check for items array (simple check)
        if (body.find("\"items\"") == std::string::npos) {
            validation.add_error("items", "required");
        } else if (body.find("\"items\":[]") != std::string::npos) {
            validation.add_error("items", "must not be empty");
        }

        if (!validation.valid) {
            res.bad_request().json(validation.to_json());
            return;
        }

        // Simulate order processing
        double subtotal = 99.99;
        double tax = subtotal * 0.08;
        double shipping = express_shipping ? 15.00 : 5.00;
        double total = subtotal + tax + shipping;

        JsonBuilder builder;
        builder.object()
            .key("order_id").value("ORD-" + std::to_string(rand() % 100000))
            .key("customer").value(customer)
            .key("status").value("confirmed")
            .key("pricing").object()
                .key("subtotal").value(subtotal)
                .key("tax").value(tax)
                .key("shipping").value(shipping)
                .key("total").value(total)
            .end_object()
            .key("express_shipping").value(express_shipping)
            .key("estimated_delivery").value(express_shipping ? "2-3 days" : "5-7 days")
        .end_object();

        res.created().json(builder.build());
    });

    // Complex query with typed response
    app.get("/api/analytics", [](Request& req, Response& res) {
        std::string period = req.query_param_optional("period").value_or("7d");
        std::string metric = req.query_param_optional("metric").value_or("all");

        JsonBuilder builder;
        builder.object()
            .key("period").value(period)
            .key("metric").value(metric)
            .key("data").object()
                .key("visitors").value(12543)
                .key("page_views").value(45678)
                .key("bounce_rate").value(0.35)
                .key("avg_session_duration").value(245.5)
            .end_object()
            .key("trends").array()
                .object().key("date").value("2024-01-01").key("value").value(1200).end_object()
                .object().key("date").value("2024-01-02").key("value").value(1350).end_object()
                .object().key("date").value("2024-01-03").key("value").value(1500).end_object()
            .end_array()
            .key("meta").object()
                .key("generated_at").value("2024-01-03T12:00:00Z")
                .key("cached").value(false)
            .end_object()
        .end_object();

        res.json(builder.build());
    });

    // Batch operations
    app.post("/api/batch", [](Request& req, Response& res) {
        std::string body = req.body();

        JsonBuilder builder;
        builder.object()
            .key("results").array()
                .object()
                    .key("operation").value("create")
                    .key("status").value("success")
                    .key("id").value(1)
                .end_object()
                .object()
                    .key("operation").value("update")
                    .key("status").value("success")
                    .key("id").value(2)
                .end_object()
                .object()
                    .key("operation").value("delete")
                    .key("status").value("failed")
                    .key("error").value("Not found")
                .end_object()
            .end_array()
            .key("summary").object()
                .key("total").value(3)
                .key("succeeded").value(2)
                .key("failed").value(1)
            .end_object()
        .end_object();

        res.json(builder.build());
    });

    // Error response examples
    app.get("/api/error-examples/{type}", [](Request& req, Response& res) {
        std::string type = req.path_param("type");

        if (type == "not-found") {
            JsonBuilder b;
            b.object()
                .key("error").value("Not Found")
                .key("message").value("The requested resource does not exist")
                .key("code").value("RESOURCE_NOT_FOUND")
            .end_object();
            res.not_found().json(b.build());
        } else if (type == "unauthorized") {
            JsonBuilder b;
            b.object()
                .key("error").value("Unauthorized")
                .key("message").value("Authentication required")
                .key("code").value("AUTH_REQUIRED")
            .end_object();
            res.unauthorized().json(b.build());
        } else if (type == "forbidden") {
            JsonBuilder b;
            b.object()
                .key("error").value("Forbidden")
                .key("message").value("You don't have permission to access this resource")
                .key("code").value("ACCESS_DENIED")
            .end_object();
            res.forbidden().json(b.build());
        } else if (type == "server-error") {
            JsonBuilder b;
            b.object()
                .key("error").value("Internal Server Error")
                .key("message").value("An unexpected error occurred")
                .key("code").value("INTERNAL_ERROR")
                .key("request_id").value("req-" + std::to_string(rand() % 100000))
            .end_object();
            res.internal_error().json(b.build());
        } else {
            res.json(R"({"available_types":["not-found","unauthorized","forbidden","server-error"]})");
        }
    });

    std::cout << "\nStarting on http://localhost:8080" << std::endl;
    std::cout << "\nEndpoints:" << std::endl;
    std::cout << "  POST /api/register  - User registration with validation" << std::endl;
    std::cout << "  POST /api/order     - Order creation" << std::endl;
    std::cout << "  GET  /api/analytics - Analytics data" << std::endl;
    std::cout << "  POST /api/batch     - Batch operations" << std::endl;
    std::cout << "  GET  /api/error-examples/{type} - Error response examples" << std::endl;
    std::cout << std::endl;

    return app.run_unified("0.0.0.0", 8080);
}
