#include "response.h"
#include <cstring>
#include <algorithm>

// uWebSockets integration disabled for now

// Default constructor
HttpResponse::HttpResponse() 
    : status_(Status::OK), type_(Type::TEXT), is_streaming_(false), 
      is_sent_(false), compression_enabled_(true), compression_level_(3),
      original_size_(0), compressed_size_(0) {
}

// uWebSockets constructor disabled for now

HttpResponse::~HttpResponse() = default;

HttpResponse& HttpResponse::status(Status status) noexcept {
    status_ = status;
    return *this;
}

HttpResponse& HttpResponse::header(const std::string& name, const std::string& value) noexcept {
    headers_[name] = value;
    return *this;
}

HttpResponse& HttpResponse::content_type(const std::string& content_type) noexcept {
    return header("content-type", content_type);
}

HttpResponse& HttpResponse::json(const std::string& data) noexcept {
    body_ = data;
    return content_type("application/json");
}

HttpResponse& HttpResponse::text(const std::string& text) noexcept {
    body_ = text;
    return content_type("text/plain");
}

HttpResponse& HttpResponse::html(const std::string& html) noexcept {
    body_ = html;
    return content_type("text/html");
}

HttpResponse& HttpResponse::binary(const std::vector<uint8_t>& data) noexcept {
    binary_body_ = data;
    return content_type("application/octet-stream");
}

HttpResponse& HttpResponse::file(const std::string& file_path) noexcept {
    file_path_ = file_path;
    return content_type("application/octet-stream");
}

HttpResponse& HttpResponse::stream(const std::string& content_type) noexcept {
    is_streaming_ = true;
    return this->content_type(content_type);
}

HttpResponse& HttpResponse::write(const std::string& data) noexcept {
    body_ += data;
    return *this;
}

HttpResponse& HttpResponse::write(const std::vector<uint8_t>& data) noexcept {
    binary_body_.insert(binary_body_.end(), data.begin(), data.end());
    return *this;
}

HttpResponse& HttpResponse::end() noexcept {
    is_streaming_ = false;
    return *this;
}

HttpResponse& HttpResponse::compress(bool enable) noexcept {
    compression_enabled_ = enable;
    return *this;
}

HttpResponse& HttpResponse::compression_level(int level) noexcept {
    compression_level_ = level;
    return *this;
}

HttpResponse& HttpResponse::redirect(const std::string& url, bool permanent) noexcept {
    status_ = permanent ? Status::MOVED_PERMANENTLY : Status::FOUND;
    return header("location", url);
}

HttpResponse& HttpResponse::cookie(
    const std::string& name, 
    const std::string& value, 
    const std::unordered_map<std::string, std::string>& options
) noexcept {
    std::string cookie_str = name + "=" + value;
    for (const auto& [key, val] : options) {
        cookie_str += "; " + key + "=" + val;
    }
    cookies_.push_back(cookie_str);
    return *this;
}

HttpResponse& HttpResponse::clear_cookie(const std::string& name, const std::string& path) noexcept {
    return cookie(name, "", {{"path", path}, {"expires", "Thu, 01 Jan 1970 00:00:00 GMT"}});
}

int HttpResponse::send() noexcept {
    if (is_sent_) return 0;
    
    original_size_ = body_.length() + binary_body_.size();
    
    // uWebSockets integration disabled for now
    
    // Fallback implementation
    if (compression_enabled_ && original_size_ > 1024) {
        compressed_size_ = original_size_;  // Placeholder
        headers_["content-encoding"] = "zstd";
    } else {
        compressed_size_ = original_size_;
    }
    
    is_sent_ = true;
    return 0;
}

bool HttpResponse::is_sent() const noexcept {
    return is_sent_;
}

uint64_t HttpResponse::get_size() const noexcept {
    return body_.length() + binary_body_.size();
}

double HttpResponse::get_compression_ratio() const noexcept {
    if (original_size_ == 0) return 0.0;
    return 1.0 - (double(compressed_size_) / double(original_size_));
}

int HttpResponse::apply_compression() noexcept {
    // Simplified compression implementation
    return 0;
}

std::string HttpResponse::serialize_headers() const noexcept {
    std::string result;
    for (const auto& [name, value] : headers_) {
        result += name + ": " + value + "\r\n";
    }
    return result;
}

std::string HttpResponse::get_status_text(Status status) const noexcept {
    switch (status) {
        case Status::OK: return "OK";
        case Status::CREATED: return "Created";
        case Status::ACCEPTED: return "Accepted";
        case Status::NO_CONTENT: return "No Content";
        case Status::MOVED_PERMANENTLY: return "Moved Permanently";
        case Status::FOUND: return "Found";
        case Status::NOT_MODIFIED: return "Not Modified";
        case Status::BAD_REQUEST: return "Bad Request";
        case Status::UNAUTHORIZED: return "Unauthorized";
        case Status::FORBIDDEN: return "Forbidden";
        case Status::NOT_FOUND: return "Not Found";
        case Status::METHOD_NOT_ALLOWED: return "Method Not Allowed";
        case Status::CONFLICT: return "Conflict";
        case Status::UNPROCESSABLE_ENTITY: return "Unprocessable Entity";
        case Status::INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case Status::NOT_IMPLEMENTED: return "Not Implemented";
        case Status::BAD_GATEWAY: return "Bad Gateway";
        case Status::SERVICE_UNAVAILABLE: return "Service Unavailable";
        default: return "Unknown";
    }
}

std::string HttpResponse::to_http_wire_format(bool keep_alive) const noexcept {
    std::string response;
    response.reserve(512 + body_.size() + binary_body_.size());

    // Status line: "HTTP/1.1 200 OK\r\n"
    response += "HTTP/1.1 ";
    response += std::to_string(static_cast<int>(status_));
    response += " ";
    response += get_status_text(status_);
    response += "\r\n";

    // Content-Type header (if set or inferred from type)
    std::string content_type = content_type_;
    if (content_type.empty()) {
        auto ct_it = headers_.find("content-type");
        if (ct_it != headers_.end()) {
            content_type = ct_it->second;
        }
    }
    if (!content_type.empty()) {
        response += "Content-Type: ";
        response += content_type;
        response += "\r\n";
    }

    // Content-Length header
    size_t content_length = !binary_body_.empty() ? binary_body_.size() : body_.size();
    response += "Content-Length: ";
    response += std::to_string(content_length);
    response += "\r\n";

    // Connection header
    response += "Connection: ";
    response += keep_alive ? "keep-alive" : "close";
    response += "\r\n";

    // Additional headers (skip content-type and content-length if already set)
    for (const auto& [name, value] : headers_) {
        // Case-insensitive comparison for content-type and content-length
        std::string name_lower = name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        if (name_lower != "content-type" && name_lower != "content-length" && name_lower != "connection") {
            response += name;
            response += ": ";
            response += value;
            response += "\r\n";
        }
    }

    // Cookies
    for (const auto& cookie : cookies_) {
        response += "Set-Cookie: ";
        response += cookie;
        response += "\r\n";
    }

    // End of headers
    response += "\r\n";

    // Body
    if (!binary_body_.empty()) {
        response.append(reinterpret_cast<const char*>(binary_body_.data()), binary_body_.size());
    } else {
        response += body_;
    }

    return response;
}
