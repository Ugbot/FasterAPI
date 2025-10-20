#include "request.h"
#include <cstring>
#include <chrono>
#include <random>

// uWebSockets integration disabled for now

// Default constructor
HttpRequest::HttpRequest() noexcept 
    : method_(Method::GET), path_("/"), query_(""), version_("HTTP/1.1"), 
      protocol_("HTTP/1.1"), client_ip_("127.0.0.1"), request_id_(0), 
      timestamp_(0), secure_(false) {
    // Generate unique request ID
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    request_id_ = gen();
    timestamp_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// uWebSockets constructor disabled for now

HttpRequest::Method HttpRequest::get_method() const noexcept {
    return method_;
}

const std::string& HttpRequest::get_path() const noexcept {
    return path_;
}

const std::string& HttpRequest::get_query() const noexcept {
    return query_;
}

const std::string& HttpRequest::get_version() const noexcept {
    return version_;
}

std::string HttpRequest::get_header(const std::string& name) const noexcept {
    auto it = headers_.find(name);
    return it != headers_.end() ? it->second : "";
}

const std::unordered_map<std::string, std::string>& HttpRequest::get_headers() const noexcept {
    return headers_;
}

std::string HttpRequest::get_query_param(const std::string& name) const noexcept {
    auto it = query_params_.find(name);
    return it != query_params_.end() ? it->second : "";
}

std::string HttpRequest::get_path_param(const std::string& name) const noexcept {
    auto it = path_params_.find(name);
    return it != path_params_.end() ? it->second : "";
}

const std::string& HttpRequest::get_body() const noexcept {
    return body_;
}

const std::vector<uint8_t>& HttpRequest::get_body_bytes() const noexcept {
    return body_bytes_;
}

std::string HttpRequest::get_content_type() const noexcept {
    return get_header("content-type");
}

uint64_t HttpRequest::get_content_length() const noexcept {
    std::string length_str = get_header("content-length");
    if (length_str.empty()) return 0;
    
    // Simple string to number conversion without exceptions
    uint64_t result = 0;
    for (char c : length_str) {
        if (c >= '0' && c <= '9') {
            result = result * 10 + (c - '0');
        } else {
            break;
        }
    }
    return result;
}

bool HttpRequest::is_json() const noexcept {
    std::string content_type = get_content_type();
    return content_type.find("application/json") != std::string::npos;
}

bool HttpRequest::is_multipart() const noexcept {
    std::string content_type = get_content_type();
    return content_type.find("multipart/form-data") != std::string::npos;
}

const std::string& HttpRequest::get_client_ip() const noexcept {
    return client_ip_;
}

std::string HttpRequest::get_user_agent() const noexcept {
    return get_header("user-agent");
}

uint64_t HttpRequest::get_request_id() const noexcept {
    return request_id_;
}

uint64_t HttpRequest::get_timestamp() const noexcept {
    return timestamp_;
}

bool HttpRequest::is_secure() const noexcept {
    return secure_;
}

const std::string& HttpRequest::get_protocol() const noexcept {
    return protocol_;
}

void HttpRequest::parse_query_params() noexcept {
    // Simple query parameter parsing
    if (query_.empty()) return;
    
    size_t pos = 0;
    while (pos < query_.length()) {
        size_t eq_pos = query_.find('=', pos);
        if (eq_pos == std::string::npos) break;
        
        size_t amp_pos = query_.find('&', eq_pos);
        if (amp_pos == std::string::npos) amp_pos = query_.length();
        
        std::string key = query_.substr(pos, eq_pos - pos);
        std::string value = query_.substr(eq_pos + 1, amp_pos - eq_pos - 1);
        
        query_params_[key] = value;
        pos = amp_pos + 1;
    }
}

std::string HttpRequest::parse_content_type() const noexcept {
    return get_header("content-type");
}
