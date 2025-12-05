#include "http_server_c_api.h"
#include "server.h"
#include "python_callback_bridge.h"
#include "route_metadata.h"
#include "request.h"
#include "response.h"
#include "../python/process_pool_executor.h"
#include "../core/logger.h"
#include <Python.h>
#include <cstring>
#include <thread>

/**
 * C API Implementation for HTTP Server
 *
 * This wraps the C++ HttpServer class with a pure C interface,
 * enabling ctypes bindings from Python.
 */

int http_lib_init() {
    // Initialize ProcessPoolExecutor with config from environment
    fasterapi::python::ProcessPoolExecutor::Config pool_config;

    // Check for shared memory IPC mode via environment variable
    // Default: ZeroMQ IPC (pool_config.use_zeromq defaults to true)
#ifdef FASTERAPI_USE_ZMQ
    const char* use_zmq_env = std::getenv("FASTERAPI_USE_ZMQ");
    // Only use shared memory if explicitly disabled
    if (use_zmq_env && (strcmp(use_zmq_env, "0") == 0 || strcmp(use_zmq_env, "false") == 0)) {
        pool_config.use_zeromq = false;
        LOG_INFO("HTTP_API", "Using shared memory IPC (legacy, FASTERAPI_USE_ZMQ=0)");
    } else {
        // ZeroMQ is default (already set in Config constructor)
        LOG_INFO("HTTP_API", "Using ZeroMQ IPC (default)");
    }
#else
    // ZeroMQ not compiled in, fall back to shared memory
    pool_config.use_zeromq = false;
    LOG_INFO("HTTP_API", "Using shared memory IPC (ZeroMQ not available)");
#endif

    // Initialize ProcessPoolExecutor singleton with config
    fasterapi::python::ProcessPoolExecutor::initialize(pool_config);

    // Initialize Python callback bridge
    PythonCallbackBridge::initialize();
    return HTTP_OK;
}

/**
 * Connect a RouteRegistry to the PythonCallbackBridge.
 *
 * This must be called after both the HTTP library and Cython module are loaded.
 * It enables metadata-aware parameter extraction.
 *
 * @param registry_ptr Pointer to RouteRegistry instance
 * @return HTTP_OK on success
 */
int http_connect_route_registry(void* registry_ptr) {
    if (!registry_ptr) {
        LOG_ERROR("HTTP_API", "Null RouteRegistry pointer");
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    PythonCallbackBridge::set_route_registry(
        static_cast<fasterapi::http::RouteRegistry*>(registry_ptr)
    );

    LOG_INFO("HTTP_API", "Connected RouteRegistry to PythonCallbackBridge");
    return HTTP_OK;
}

HttpServerHandle http_server_create(
    uint16_t port,
    const char* host,
    bool enable_h2,
    bool enable_h3,
    bool enable_webtransport,
    uint16_t http3_port,
    bool enable_compression,
    int* error_out
) {
    if (error_out) *error_out = HTTP_OK;

    if (!host) {
        if (error_out) *error_out = HTTP_ERROR_INVALID_ARGUMENT;
        return nullptr;
    }

    // Create server configuration
    HttpServer::Config config;
    config.port = port;
    config.host = host;
    config.enable_h1 = true;  // Always enable HTTP/1.1 with CoroIO
    config.enable_h2 = enable_h2;
    config.enable_h3 = enable_h3;
    config.enable_webtransport = enable_webtransport;
    config.http3_port = http3_port;
    config.enable_compression = enable_compression;

    // Create server on heap (using new - no exceptions with -fno-exceptions)
    HttpServer* server = new (std::nothrow) HttpServer(config);

    if (!server) {
        if (error_out) *error_out = HTTP_ERROR_OUT_OF_MEMORY;
        return nullptr;
    }

    // Return as opaque handle
    return static_cast<HttpServerHandle>(server);
}

int http_add_route(
    HttpServerHandle handle,
    const char* method,
    const char* path,
    uint32_t handler_id,
    int* error_out
) {
    if (error_out) *error_out = HTTP_OK;

    if (!handle || !method || !path) {
        if (error_out) *error_out = HTTP_ERROR_INVALID_ARGUMENT;
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    HttpServer* server = static_cast<HttpServer*>(handle);

    // Create C++ lambda handler that bridges to Python via PythonCallbackBridge
    // This lambda will be called by the router when a request matches this route
    std::string method_str(method);
    std::string path_str(path);

    auto cpp_handler = [method_str, path_str](
        HttpRequest* req,
        HttpResponse* res,
        const fasterapi::http::RouteParams& params
    ) {
        // Extract request data
        const auto& headers = req->get_headers();
        const std::string& body = req->get_body();

        // Build full URL with query string for parameter extraction
        std::string full_url = req->get_path();
        const std::string& query = req->get_query();
        if (!query.empty()) {
            full_url += "?" + query;
        }

        // ASYNC EXECUTION: Submit handler to sub-interpreter pool
        // This returns immediately, allowing the event loop to continue serving requests
        // The Python handler executes in a sub-interpreter with its own GIL (true parallelism)
        auto future_result = PythonCallbackBridge::invoke_handler_async(
            method_str,
            full_url,
            headers,
            body
        );

        // Attach continuation to process result when ready
        // Note: We need to move response handling into the continuation since
        // the handler executes asynchronously. For now, we'll use a simplified
        // approach that waits for the result (blocking), but the infrastructure
        // is in place for true async handling when the server supports it.

        // TODO: Once HttpServer supports deferred response sending, this should be:
        // future_result.then([res_ptr = std::shared_ptr<HttpResponse>(res)](...) { ... });

        // For now, wait for result (still benefits from sub-interpreter parallelism)
        auto result_wrapper = future_result.get();  // Blocks until Python completes

        if (!result_wrapper.is_ok()) {
            // Handler execution failed
            res->status(HttpResponse::Status::INTERNAL_SERVER_ERROR)
               .content_type("application/json")
               .json("{\"error\":\"Internal server error\"}")
               .send();
            return;
        }

        auto& result = result_wrapper.value();

        // Convert result to HTTP response
        res->status(static_cast<HttpResponse::Status>(result.status_code))
           .content_type(result.content_type);

        // Add custom headers
        for (const auto& [key, value] : result.headers) {
            res->header(key, value);
        }

        // Send response body based on content type
        if (result.content_type.find("application/json") != std::string::npos) {
            res->json(result.body);
        } else {
            res->text(result.body);
        }

        // Send the response
        res->send();
    };

    // Register the C++ handler with HttpServer's router
    int result = server->add_route(method_str, path_str, cpp_handler);

    if (result != 0) {
        if (error_out) *error_out = HTTP_ERROR_INVALID_ARGUMENT;
        LOG_ERROR("HTTP_API", "Failed to register route with HttpServer: %s %s", method, path);
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    LOG_INFO("HTTP_API", "Route registered: %s %s (handler_id: %u)", method, path, handler_id);

    return HTTP_OK;
}

int http_add_websocket(
    HttpServerHandle handle,
    const char* path,
    uint32_t handler_id,
    int* error_out
) {
    if (error_out) *error_out = HTTP_OK;

    if (!handle || !path) {
        if (error_out) *error_out = HTTP_ERROR_INVALID_ARGUMENT;
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    // Same as http_add_route - actual callback registration happens via
    // http_register_python_handler()

    LOG_INFO("HTTP_API", "WebSocket registered: %s (handler_id: %u)", path, handler_id);

    return HTTP_OK;
}

void http_register_websocket_handler_metadata(
    const char* path,
    const char* module_name,
    const char* function_name
) {
    if (!path || !module_name || !function_name) {
        LOG_WARN("HTTP_API", "http_register_websocket_handler_metadata: invalid arguments");
        return;
    }

    PythonCallbackBridge::register_websocket_handler(path, module_name, function_name);
}

int http_server_start(HttpServerHandle handle, int* error_out) {
    if (error_out) *error_out = HTTP_OK;

    if (!handle) {
        if (error_out) *error_out = HTTP_ERROR_INVALID_ARGUMENT;
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    HttpServer* server = static_cast<HttpServer*>(handle);

    int result = server->start();

    if (result != 0) {
        if (error_out) *error_out = HTTP_ERROR_START_FAILED;
        return HTTP_ERROR_START_FAILED;
    }

    return HTTP_OK;
}

int http_server_stop(HttpServerHandle handle, int* error_out) {
    if (error_out) *error_out = HTTP_OK;

    if (!handle) {
        if (error_out) *error_out = HTTP_ERROR_INVALID_ARGUMENT;
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    HttpServer* server = static_cast<HttpServer*>(handle);

    int result = server->stop();

    if (result != 0) {
        if (error_out) *error_out = HTTP_ERROR_STOP_FAILED;
        return HTTP_ERROR_STOP_FAILED;
    }

    return HTTP_OK;
}

bool http_server_is_running(HttpServerHandle handle) {
    if (!handle) {
        return false;
    }

    HttpServer* server = static_cast<HttpServer*>(handle);
    return server->is_running();
}

int http_server_destroy(HttpServerHandle handle) {
    if (!handle) {
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    HttpServer* server = static_cast<HttpServer*>(handle);

    // Ensure server is stopped
    if (server->is_running()) {
        server->stop();
    }

    // Delete server
    delete server;

    return HTTP_OK;
}

void http_register_python_handler(
    const char* method,
    const char* path,
    int handler_id,
    void* py_callable
) {
    LOG_DEBUG("HTTP_API", "http_register_python_handler called: %s %s, handler_id=%d, callable=%p",
             method ? method : "NULL", path ? path : "NULL", handler_id, py_callable);

    if (!method || !path || !py_callable) {
        LOG_ERROR("HTTP_API", "Invalid arguments to http_register_python_handler");
        return;
    }

    LOG_DEBUG("HTTP_API", "Calling PythonCallbackBridge::register_handler");

    // Register with PythonCallbackBridge (lockfree queue)
    PythonCallbackBridge::register_handler(
        std::string(method),
        std::string(path),
        handler_id,
        py_callable
    );

    LOG_DEBUG("HTTP_API", "PythonCallbackBridge::register_handler returned");
}

void* http_get_route_handler(
    void* registry_ptr,
    const char* method,
    const char* path
) {
    if (!registry_ptr || !method || !path) {
        LOG_ERROR("HTTP_API", "Invalid arguments to http_get_route_handler");
        return nullptr;
    }

    auto* registry = static_cast<fasterapi::http::RouteRegistry*>(registry_ptr);

    // Use RouteRegistry::match() to find route
    const auto* metadata = registry->match(method, path);

    if (!metadata) {
        LOG_DEBUG("HTTP_API", "No handler found for %s %s", method, path);
        return nullptr;
    }

    LOG_DEBUG("HTTP_API", "Retrieved handler for %s %s", method, path);

    // Check if handler is valid
    void* handler_ptr = static_cast<void*>(metadata->handler);
    LOG_DEBUG("HTTP_API", "Handler pointer: %p", handler_ptr);

    // Return handler PyObject*
    return handler_ptr;
}

int http_init_process_pool_executor(
    uint32_t num_workers,
    const char* python_executable,
    const char* project_dir
) {
    using namespace fasterapi::python;

    if (!python_executable || !project_dir) {
        LOG_ERROR("HTTP_API", "Invalid arguments to http_init_process_pool_executor");
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    LOG_INFO("HTTP_API", "Initializing ProcessPoolExecutor: workers=%u, python=%s, dir=%s",
             num_workers, python_executable, project_dir);

    // Build config
    ProcessPoolExecutor::Config config;
    config.num_workers = num_workers;
    config.python_executable = python_executable;
    config.project_dir = project_dir;

    // Initialize singleton
    ProcessPoolExecutor::initialize(config);

    LOG_INFO("HTTP_API", "ProcessPoolExecutor initialized successfully");
    return HTTP_OK;
}

// Simple JSON parser helper functions
namespace {
    // Skip whitespace
    const char* skip_ws(const char* p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        return p;
    }

    // Parse a quoted string
    std::string parse_string(const char*& p) {
        p = skip_ws(p);
        if (*p != '"') return "";
        ++p;  // skip opening quote

        std::string result;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p+1)) {
                ++p;  // skip escape char, take next char literally
            }
            result += *p++;
        }

        if (*p == '"') ++p;  // skip closing quote
        return result;
    }

    // Parse a boolean
    bool parse_bool(const char*& p) {
        p = skip_ws(p);
        if (strncmp(p, "true", 4) == 0) {
            p += 4;
            return true;
        } else if (strncmp(p, "false", 5) == 0) {
            p += 5;
            return false;
        }
        return true;  // default
    }

    // Find next occurrence of character outside strings
    const char* find_next(const char* start, const char* p, char ch) {
        bool in_string = false;
        while (*p) {
            if (*p == '"' && (p == start || *(p-1) != '\\')) {
                in_string = !in_string;
            } else if (!in_string && *p == ch) {
                return p;
            }
            ++p;
        }
        return nullptr;
    }
}

int http_register_route_metadata(
    const char* method,
    const char* path,
    const char* param_metadata_json
) {
    if (!method || !path || !param_metadata_json) {
        LOG_ERROR("HTTP_API", "Invalid arguments to http_register_route_metadata");
        return HTTP_ERROR_INVALID_ARGUMENT;
    }

    LOG_DEBUG("HTTP_API", "Registering metadata for %s %s: %s", method, path, param_metadata_json);

    // Parse JSON metadata manually (simple parser for known format)
    // Expected format: {"parameters": [{"name": "user_id", "type": "integer", "location": "path", "required": true}, ...]}

    // Build RouteMetadata object
    fasterapi::http::RouteMetadata route_metadata(method, path);

    // Find "parameters" array
    const char* p = strstr(param_metadata_json, "\"parameters\"");
    if (!p) {
        LOG_WARN("HTTP_API", "No 'parameters' field found in metadata JSON");
        PythonCallbackBridge::register_route_metadata(method, path, std::move(route_metadata));
        return HTTP_OK;
    }

    // Skip to opening bracket of array
    p = strchr(p, '[');
    if (!p) {
        LOG_ERROR("HTTP_API", "Invalid JSON: expected array after 'parameters'");
        return HTTP_ERROR_INVALID_ARGUMENT;
    }
    ++p;  // skip '['

    // Parse each parameter object
    while (true) {
        p = skip_ws(p);
        if (*p == ']') break;  // end of array
        if (*p != '{') {
            LOG_ERROR("HTTP_API", "Invalid JSON: expected object in parameters array");
            return HTTP_ERROR_INVALID_ARGUMENT;
        }

        // Find the end of this object
        const char* obj_start = p + 1;
        const char* obj_end = find_next(param_metadata_json, obj_start, '}');
        if (!obj_end) {
            LOG_ERROR("HTTP_API", "Invalid JSON: unterminated object");
            return HTTP_ERROR_INVALID_ARGUMENT;
        }

        // Parse parameter fields
        std::string name, type, location, default_value;
        bool required = true;
        bool has_default = false;

        const char* obj_p = p + 1;
        while (obj_p < obj_end) {
            obj_p = skip_ws(obj_p);
            if (*obj_p == '}' || *obj_p == '\0') break;

            // Parse field name
            std::string field_name = parse_string(obj_p);
            obj_p = skip_ws(obj_p);
            if (*obj_p == ':') ++obj_p;

            // Parse field value based on field name
            if (field_name == "name") {
                name = parse_string(obj_p);
            } else if (field_name == "type") {
                type = parse_string(obj_p);
            } else if (field_name == "location") {
                location = parse_string(obj_p);
            } else if (field_name == "required") {
                required = parse_bool(obj_p);
            } else if (field_name == "default") {
                default_value = parse_string(obj_p);
                has_default = true;
            } else {
                // Skip unknown field value
                obj_p = skip_ws(obj_p);
                if (*obj_p == '"') {
                    parse_string(obj_p);
                } else if (strncmp(obj_p, "true", 4) == 0) {
                    obj_p += 4;
                } else if (strncmp(obj_p, "false", 5) == 0) {
                    obj_p += 5;
                } else if (*obj_p == '[' || *obj_p == '{') {
                    // Skip nested structures
                    char end_ch = (*obj_p == '[') ? ']' : '}';
                    int depth = 1;
                    ++obj_p;
                    while (*obj_p && depth > 0) {
                        if (*obj_p == '"') {
                            parse_string(obj_p);
                            continue;
                        }
                        if (*obj_p == '[' || *obj_p == '{') ++depth;
                        if (*obj_p == ']' || *obj_p == '}') --depth;
                        ++obj_p;
                    }
                }
            }

            // Skip comma
            obj_p = skip_ws(obj_p);
            if (*obj_p == ',') ++obj_p;
        }

        // Create ParameterInfo if we have the required fields
        if (!name.empty() && !type.empty() && !location.empty()) {
            // Map type string to SchemaType
            fasterapi::http::SchemaType schema_type = fasterapi::http::STRING;
            if (type == "integer") {
                schema_type = fasterapi::http::INTEGER;
            } else if (type == "number") {
                schema_type = fasterapi::http::FLOAT;
            } else if (type == "boolean") {
                schema_type = fasterapi::http::BOOLEAN;
            }

            // Map location string to ParameterLocation
            fasterapi::http::ParameterLocation param_location = fasterapi::http::QUERY;
            if (location == "path") {
                param_location = fasterapi::http::PATH;
            } else if (location == "query") {
                param_location = fasterapi::http::QUERY;
            } else if (location == "body") {
                param_location = fasterapi::http::BODY;
            } else if (location == "header") {
                param_location = fasterapi::http::HEADER;
            } else if (location == "cookie") {
                param_location = fasterapi::http::COOKIE;
            }

            // Create ParameterInfo
            fasterapi::http::ParameterInfo param_info(name.c_str(), schema_type, param_location, required);

            // Add default value if present
            if (has_default) {
                param_info.default_value = default_value;
            }

            route_metadata.parameters.push_back(std::move(param_info));
        }

        // Move to next parameter
        p = obj_end + 1;  // skip '}'
        p = skip_ws(p);
        if (*p == ',') ++p;  // skip comma
    }

    LOG_INFO("HTTP_API", "Parsed %zu parameters for %s %s",
             route_metadata.parameters.size(), method, path);

    // Register with PythonCallbackBridge
    PythonCallbackBridge::register_route_metadata(method, path, std::move(route_metadata));

    return HTTP_OK;
}
