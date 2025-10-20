#include "native_request.h"
#include "../http/http1_parser.h"
#include <simdjson.h>
#include <cstring>

namespace fasterapi {
namespace types {

// ============================================================================
// NativeRequest Implementation
// ============================================================================

PyTypeObject NativeRequest::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "fasterapi.NativeRequest",
    sizeof(NativeRequest),
    0, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT,
    "Native HTTP request (zero-copy)",
};

NativeRequest* NativeRequest::create_from_buffer(
    const uint8_t* buffer,
    size_t len
) noexcept {
    auto* obj = PyObject_New(NativeRequest, &Type);
    if (!obj) {
        return nullptr;
    }
    
    // Store buffer reference (for zero-copy views)
    obj->buffer = buffer;
    obj->buffer_len = len;
    
    // Parse with our HTTP/1.1 parser (zero-copy!)
    // For now, simplified - production would parse fully
    obj->method = std::string_view("GET");
    obj->path = std::string_view("/");
    obj->version = std::string_view("HTTP/1.1");
    
    obj->headers = nullptr;
    obj->header_count = 0;
    obj->params = nullptr;
    obj->param_count = 0;
    obj->query_params = nullptr;
    obj->query_param_count = 0;
    
    return obj;
}

std::string_view NativeRequest::get_header(std::string_view name) const noexcept {
    // Linear search (fast for small header counts)
    for (uint32_t i = 0; i < header_count; ++i) {
        if (headers[i].name == name) {
            return headers[i].value;
        }
    }
    return {};
}

std::string_view NativeRequest::get_param(std::string_view name) const noexcept {
    for (uint32_t i = 0; i < param_count; ++i) {
        if (params[i].name == name) {
            return params[i].value;
        }
    }
    return {};
}

std::string_view NativeRequest::get_query_param(std::string_view name) const noexcept {
    for (uint32_t i = 0; i < query_param_count; ++i) {
        if (query_params[i].name == name) {
            return query_params[i].value;
        }
    }
    return {};
}

NativeDict* NativeRequest::json() noexcept {
    // Parse body as JSON using simdjson
    // Return NativeDict (not PyDict!)
    
    if (body.empty()) {
        return NativeDict::create();
    }
    
    // Use simdjson to parse
    static simdjson::ondemand::parser parser;
    
    simdjson::padded_string padded(body.data(), body.length());
    simdjson::ondemand::document doc;
    
    auto error = parser.iterate(padded).get(doc);
    if (error) {
        return NativeDict::create();
    }
    
    // Convert to NativeDict
    // For now, simplified - production would recursively convert
    NativeDict* dict = NativeDict::create();
    
    return dict;
}

PyObject* NativeRequest::to_python() const noexcept {
    // Convert to Python Request object (when needed)
    // For now, return a simple dict
    PyObject* dict = PyDict_New();
    
    PyDict_SetItemString(dict, "method", PyUnicode_FromStringAndSize(method.data(), method.length()));
    PyDict_SetItemString(dict, "path", PyUnicode_FromStringAndSize(path.data(), path.length()));
    
    return dict;
}

// ============================================================================
// NativeResponse Implementation
// ============================================================================

PyTypeObject NativeResponse::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "fasterapi.NativeResponse",
    sizeof(NativeResponse),
    0, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    Py_TPFLAGS_DEFAULT,
    "Native HTTP response",
};

NativeResponse* NativeResponse::create() noexcept {
    auto* obj = PyObject_New(NativeResponse, &Type);
    if (!obj) {
        return nullptr;
    }
    
    obj->status_code = 200;
    obj->body_capacity = 4096;
    obj->body_size = 0;
    obj->body_buffer = new uint8_t[obj->body_capacity];
    obj->content_type = "application/json";
    
    return obj;
}

void NativeResponse::set_status(uint16_t status) noexcept {
    status_code = status;
}

void NativeResponse::set_header(std::string_view name, std::string_view value) noexcept {
    headers.push_back({std::string(name), std::string(value)});
}

int NativeResponse::set_json(const NativeDict* dict) noexcept {
    // Serialize NativeDict to JSON (SIMD-optimized!)
    size_t written;
    
    if (dict->to_json(reinterpret_cast<char*>(body_buffer), body_capacity, written) != 0) {
        return 1;
    }
    
    body_size = written;
    content_type = "application/json";
    
    return 0;
}

int NativeResponse::set_text(std::string_view text) noexcept {
    if (text.length() > body_capacity) {
        return 1;
    }
    
    std::memcpy(body_buffer, text.data(), text.length());
    body_size = text.length();
    content_type = "text/plain";
    
    return 0;
}

int NativeResponse::serialize(uint8_t* output, size_t capacity, size_t& written) noexcept {
    // Build HTTP response
    size_t pos = 0;
    
    // Status line
    const char* status_line = "HTTP/1.1 200 OK\r\n";
    size_t status_len = std::strlen(status_line);
    
    if (pos + status_len >= capacity) return 1;
    std::memcpy(output + pos, status_line, status_len);
    pos += status_len;
    
    // Content-Type header
    const char* ct_header = "Content-Type: ";
    if (pos + std::strlen(ct_header) + content_type.length() + 2 >= capacity) return 1;
    std::memcpy(output + pos, ct_header, std::strlen(ct_header));
    pos += std::strlen(ct_header);
    std::memcpy(output + pos, content_type.c_str(), content_type.length());
    pos += content_type.length();
    output[pos++] = '\r';
    output[pos++] = '\n';
    
    // Content-Length
    char cl_buf[64];
    int cl_len = snprintf(cl_buf, sizeof(cl_buf), "Content-Length: %zu\r\n", body_size);
    if (pos + cl_len >= capacity) return 1;
    std::memcpy(output + pos, cl_buf, cl_len);
    pos += cl_len;
    
    // End headers
    if (pos + 2 >= capacity) return 1;
    output[pos++] = '\r';
    output[pos++] = '\n';
    
    // Body
    if (pos + body_size >= capacity) return 1;
    std::memcpy(output + pos, body_buffer, body_size);
    pos += body_size;
    
    written = pos;
    return 0;
}

PyObject* NativeResponse::to_python() const noexcept {
    PyObject* dict = PyDict_New();
    
    PyDict_SetItemString(dict, "status", PyLong_FromLong(status_code));
    PyDict_SetItemString(dict, "body", PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(body_buffer),
        body_size
    ));
    
    return dict;
}

// ============================================================================
// NativeJSON Implementation
// ============================================================================

NativeDict* NativeJSON::parse(const char* json_data, size_t len) noexcept {
    // Parse JSON with simdjson
    static simdjson::ondemand::parser parser;
    
    simdjson::padded_string padded(json_data, len);
    simdjson::ondemand::document doc;
    
    auto error = parser.iterate(padded).get(doc);
    if (error) {
        return nullptr;
    }
    
    // Convert to NativeDict (no Python objects!)
    NativeDict* dict = NativeDict::create();
    
    // TODO: Recursively convert simdjson document to NativeDict
    // For now, return empty dict
    
    return dict;
}

int NativeJSON::serialize(
    const NativeDict* dict,
    char* output,
    size_t capacity,
    size_t& written
) noexcept {
    // Use NativeDict's built-in JSON serialization
    return dict->to_json(output, capacity, written);
}

} // namespace types
} // namespace fasterapi

