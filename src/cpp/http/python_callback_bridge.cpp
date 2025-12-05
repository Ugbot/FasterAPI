#include "python_callback_bridge.h"
#include "route_metadata.h"
#include "validation_error_formatter.h"
#include "../python/process_pool_executor.h"
#include "../core/logger.h"
#include <thread>
#include <sstream>

// Static member initialization
fasterapi::core::AeronSPSCQueue<PythonCallbackBridge::HandlerRegistration>
    PythonCallbackBridge::registration_queue_(1024);

std::unordered_map<std::string, std::pair<int, PyObject*>>
    PythonCallbackBridge::handlers_;

std::unordered_map<std::string, PythonCallbackBridge::HandlerMetadata>
    PythonCallbackBridge::handler_metadata_;

fasterapi::http::RouteRegistry* PythonCallbackBridge::route_registry_ = nullptr;

std::unordered_map<std::string, fasterapi::http::RouteMetadata>
    PythonCallbackBridge::internal_route_metadata_;

std::unordered_map<std::string, PythonCallbackBridge::HandlerMetadata>
    PythonCallbackBridge::ws_handler_metadata_;

void PythonCallbackBridge::register_websocket_handler(
    const std::string& path,
    const std::string& module_name,
    const std::string& function_name
) {
    HandlerMetadata meta;
    meta.module_name = module_name;
    meta.function_name = function_name;
    meta.handler_id = -1;  // WebSocket handlers don't use handler_id
    ws_handler_metadata_[path] = std::move(meta);
    LOG_INFO("PythonCallback", "Registered WebSocket handler: %s -> %s.%s",
             path.c_str(), module_name.c_str(), function_name.c_str());
}

const PythonCallbackBridge::HandlerMetadata* PythonCallbackBridge::get_websocket_handler_metadata(
    const std::string& path
) {
    auto it = ws_handler_metadata_.find(path);
    if (it != ws_handler_metadata_.end()) {
        return &it->second;
    }
    return nullptr;
}

/**
 * Convert PyObject* response to HandlerResult.
 * Handles dicts (→JSON), strings, and other types.
 * REQUIRES: GIL must be held by caller.
 */
static PythonCallbackBridge::HandlerResult convert_python_to_handler_result(PyObject* py_response) {
    PythonCallbackBridge::HandlerResult result;

    if (!py_response) {
        result.status_code = 500;
        result.content_type = "application/json";
        result.body = "{\"error\":\"Handler returned null\"}";
        return result;
    }

    if (PyDict_Check(py_response)) {
        // Dict response - serialize to JSON
        PyObject* json_module = PyImport_ImportModule("json");
        if (json_module) {
            PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
            if (dumps) {
                PyObject* json_str = PyObject_CallFunctionObjArgs(dumps, py_response, nullptr);
                if (json_str) {
                    const char* json_cstr = PyUnicode_AsUTF8(json_str);
                    if (json_cstr) {
                        result.body = json_cstr;
                        result.content_type = "application/json";
                        result.status_code = 200;
                    }
                    Py_DECREF(json_str);
                }
                Py_DECREF(dumps);
            }
            Py_DECREF(json_module);
        }
    } else if (PyUnicode_Check(py_response)) {
        // String response
        const char* str = PyUnicode_AsUTF8(py_response);
        if (str) {
            result.body = str;
            result.content_type = "text/plain";
            result.status_code = 200;
        }
    } else {
        // Unknown type - convert to string
        PyObject* str_obj = PyObject_Str(py_response);
        if (str_obj) {
            const char* str = PyUnicode_AsUTF8(str_obj);
            if (str) {
                result.body = str;
                result.content_type = "text/plain";
                result.status_code = 200;
            }
            Py_DECREF(str_obj);
        }
    }

    return result;
}

/**
 * Extract module name and function name from a Python callable.
 *
 * This enables sub-interpreter execution by storing metadata instead of PyObject*.
 * Each sub-interpreter can import the module and get the function by name.
 */
static PythonCallbackBridge::HandlerMetadata extract_handler_metadata(PyObject* callable) {
    PythonCallbackBridge::HandlerMetadata metadata;

    if (!callable) {
        LOG_ERROR("PythonCallbackBridge", "extract_handler_metadata called with NULL callable");
        return metadata;  // Empty metadata
    }

    LOG_DEBUG("PythonCallbackBridge", "Extracting metadata from callable %p", callable);

    // Check if callable is valid by trying to get its type
    if (!PyCallable_Check(callable)) {
        LOG_ERROR("PythonCallbackBridge", "Object is not callable!");
        return metadata;
    }

    // Extract __module__ attribute
    PyObject* module_obj = PyObject_GetAttrString(callable, "__module__");
    if (module_obj) {
        if (PyUnicode_Check(module_obj)) {
            const char* module_str = PyUnicode_AsUTF8(module_obj);
            if (module_str) {
                metadata.module_name = module_str;
            }
        }
        Py_DECREF(module_obj);
    } else {
        // Clear error - not all callables have __module__
        PyErr_Clear();
        LOG_DEBUG("PythonCallbackBridge", "Callable has no __module__ attribute");
    }

    // Extract __qualname__ attribute (qualified name, includes class if method)
    PyObject* name_obj = PyObject_GetAttrString(callable, "__qualname__");
    if (name_obj) {
        if (PyUnicode_Check(name_obj)) {
            const char* name_str = PyUnicode_AsUTF8(name_obj);
            if (name_str) {
                metadata.function_name = name_str;
            }
        }
        Py_DECREF(name_obj);
    } else {
        // Fallback to __name__ if __qualname__ not available
        PyErr_Clear();
        PyObject* fallback_name = PyObject_GetAttrString(callable, "__name__");
        if (fallback_name) {
            if (PyUnicode_Check(fallback_name)) {
                const char* name_str = PyUnicode_AsUTF8(fallback_name);
                if (name_str) {
                    metadata.function_name = name_str;
                }
            }
            Py_DECREF(fallback_name);
        } else {
            PyErr_Clear();
            LOG_DEBUG("PythonCallbackBridge", "Callable has no __name__ or __qualname__ attribute");
        }
    }

    LOG_DEBUG("PythonCallbackBridge", "Extracted metadata: module=%s, function=%s",
             metadata.module_name.c_str(), metadata.function_name.c_str());

    return metadata;
}

void PythonCallbackBridge::initialize() {
    // Ensure Python is initialized
    if (!Py_IsInitialized()) {
        LOG_WARN("PythonCallbackBridge", "Python not initialized in initialize()");
    }
}

void PythonCallbackBridge::register_handler(
    const std::string& method,
    const std::string& path,
    int handler_id,
    void* callable
) {
    LOG_DEBUG("PythonCallbackBridge", "register_handler START: %s %s, handler_id=%d, callable=%p",
             method.c_str(), path.c_str(), handler_id, callable);

    // Cast void* back to PyObject*
    PyObject* py_callable = static_cast<PyObject*>(callable);

    LOG_DEBUG("PythonCallbackBridge", "About to INCREF %p", py_callable);

    // Increment reference count since we're storing it
    // Ensure GIL is held (should already be held when called from Python)
    PyGILState_STATE gstate = PyGILState_Ensure();

    Py_INCREF(py_callable);
    LOG_DEBUG("PythonCallbackBridge", "INCREF complete, about to extract metadata");

    // Extract metadata for sub-interpreter execution
    HandlerMetadata metadata = extract_handler_metadata(py_callable);
    metadata.handler_id = handler_id;

    PyGILState_Release(gstate);

    LOG_DEBUG("PythonCallbackBridge", "Metadata extracted, storing handler");

    // For initial registration (before server starts), register directly
    // The lockfree queue is only used for runtime registration after event loop is running
    // This avoids the queue filling up during initialization
    std::string key = method + ":" + path;
    handlers_[key] = std::make_pair(handler_id, py_callable);
    handler_metadata_[key] = metadata;  // Store metadata for sub-interpreter use

    LOG_INFO("PythonCallbackBridge", "Registered handler: %s (ID: %d, module=%s, func=%s)",
             key.c_str(), handler_id,
             metadata.module_name.c_str(), metadata.function_name.c_str());
}

void PythonCallbackBridge::poll_registrations() {
    // Drain the registration queue and update handlers map
    // This must be called from the event loop thread
    HandlerRegistration reg;
    int count = 0;

    while (registration_queue_.try_pop(reg)) {
        std::string key = reg.method + ":" + reg.path;
        handlers_[key] = std::make_pair(reg.handler_id, reg.callable);

        LOG_DEBUG("PythonCallbackBridge", "Activated Python handler: %s (ID: %d)", key.c_str(), reg.handler_id);
        count++;
    }

    if (count > 0) {
        LOG_DEBUG("PythonCallbackBridge", "Processed %d handler registrations", count);
    }
}

void PythonCallbackBridge::set_route_registry(fasterapi::http::RouteRegistry* registry) {
    route_registry_ = registry;
}

fasterapi::http::RouteRegistry* PythonCallbackBridge::get_route_registry() {
    return route_registry_;
}

void PythonCallbackBridge::register_route_metadata(
    const std::string& method,
    const std::string& path,
    fasterapi::http::RouteMetadata metadata
) {
    std::string route_key = method + ":" + path;
    LOG_INFO("PythonCallback", "Registering route metadata: %s (params: %zu)",
             route_key.c_str(), metadata.parameters.size());

    internal_route_metadata_[route_key] = std::move(metadata);
}

// Helper function to convert string to Python object based on SchemaType
static PyObject* convert_to_python_type(const std::string& value, fasterapi::http::SchemaType type) {
    switch (type) {
        case fasterapi::http::INTEGER: {
            // Convert string to int (using C-style conversion without exceptions)
            char* endptr = nullptr;
            long long_val = std::strtol(value.c_str(), &endptr, 10);
            if (endptr == value.c_str() || *endptr != '\0') {
                // Conversion failed - return None
                Py_RETURN_NONE;
            }
            return PyLong_FromLong(long_val);
        }
        case fasterapi::http::FLOAT: {
            // Convert string to float (using C-style conversion without exceptions)
            char* endptr = nullptr;
            double double_val = std::strtod(value.c_str(), &endptr);
            if (endptr == value.c_str()) {
                // Conversion failed - return None
                Py_RETURN_NONE;
            }
            return PyFloat_FromDouble(double_val);
        }
        case fasterapi::http::BOOLEAN: {
            // Convert string to bool
            if (value == "true" || value == "True" || value == "1") {
                Py_RETURN_TRUE;
            } else if (value == "false" || value == "False" || value == "0") {
                Py_RETURN_FALSE;
            } else {
                Py_RETURN_NONE;
            }
        }
        case fasterapi::http::STRING:
        default:
            // Return as Python string
            return PyUnicode_FromString(value.c_str());
    }
}

PythonCallbackBridge::HandlerResult PythonCallbackBridge::invoke_handler(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body
) {
    HandlerResult result;

    // Split path from query string for route matching
    // path might be "/search?q=test&limit=10", we need just "/search" for matching
    std::string route_path = path;
    size_t query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        route_path = path.substr(0, query_pos);
    }

    // Try to use RouteRegistry for metadata-aware parameter extraction
    const fasterapi::http::RouteMetadata* metadata = nullptr;
    PyObject* callable = nullptr;

    if (route_registry_) {
        // Use RouteRegistry to match route and get metadata (without query string)
        metadata = route_registry_->match(method, route_path);
        if (metadata) {
            callable = metadata->handler;
        }
    }

    // Fallback to simple map lookup if RouteRegistry not available or no match
    if (!callable) {
        // Use route_path (without query string) for handler lookup
        std::string key = method + ":" + route_path;
        auto it = handlers_.find(key);
        if (it == handlers_.end()) {
            // No handler found - return 404
            result.status_code = 404;
            result.content_type = "application/json";
            result.body = "{\"error\":\"Not Found\"}";
            return result;
        }
        callable = it->second.second;
    }

    // Acquire GIL for Python call
    PyGILState_STATE gstate = PyGILState_Ensure();

    // Build kwargs dict for FastAPI-style parameter passing
    PyObject* kwargs = PyDict_New();

    // Validate request body if metadata defines a schema
    if (metadata && !metadata->request_body_schema.empty() && !body.empty()) {
        auto schema = fasterapi::http::SchemaRegistry::instance().get_schema(metadata->request_body_schema);
        if (schema) {
            fasterapi::http::ValidationResult validation_result = schema->validate_json(body);
            if (!validation_result.valid) {
                // Return 422 Unprocessable Entity with FastAPI-format error
                Py_DECREF(kwargs);
                PyGILState_Release(gstate);

                result.status_code = 422;
                result.content_type = "application/json";
                result.body = fasterapi::http::ValidationErrorFormatter::format_as_json(validation_result);
                return result;
            }
            LOG_DEBUG("PythonCallback", "Request body validation passed for schema: %s",
                     metadata->request_body_schema.c_str());
        }
    }

    // Extract and add parameters if metadata is available
    if (metadata) {
        LOG_WARN("PythonCallback", "Extracting params from URL: %s", path.c_str());

        // 1. Extract path parameters using CompiledRoutePattern
        auto path_params = metadata->compiled_pattern.extract(route_path);
        LOG_WARN("PythonCallback", "Extracted %zu path params", path_params.size());

        // 2. Extract query parameters from URL
        auto query_params = fasterapi::http::ParameterExtractor::get_query_params(path);
        LOG_WARN("PythonCallback", "Extracted %zu query params", query_params.size());

        // 3. Build kwargs from parameters metadata
        for (const auto& param_info : metadata->parameters) {
            PyObject* py_value = nullptr;

            if (param_info.location == fasterapi::http::PATH) {
                // Look up in path parameters
                auto it = path_params.find(param_info.name);
                if (it != path_params.end()) {
                    py_value = convert_to_python_type(it->second, param_info.type);
                }
            } else if (param_info.location == fasterapi::http::QUERY) {
                // Look up in query parameters
                auto it = query_params.find(param_info.name);
                if (it != query_params.end()) {
                    py_value = convert_to_python_type(it->second, param_info.type);
                } else if (!param_info.default_value.empty()) {
                    // Use default value if parameter not found
                    py_value = convert_to_python_type(param_info.default_value, param_info.type);
                } else if (!param_info.required) {
                    // Optional parameter not provided - use None
                    Py_INCREF(Py_None);
                    py_value = Py_None;
                }
            }

            // Add to kwargs if we got a value
            if (py_value) {
                PyDict_SetItemString(kwargs, param_info.name.c_str(), py_value);
                Py_DECREF(py_value);  // Dict took a reference
            }
        }
    }

    // Add validated body to kwargs if present
    if (metadata && !body.empty()) {
        // Parse JSON body and add to kwargs
        PyObject* json_module = PyImport_ImportModule("json");
        if (json_module) {
            PyObject* loads = PyObject_GetAttrString(json_module, "loads");
            if (loads) {
                PyObject* py_body = PyObject_CallFunction(loads, "s", body.c_str());
                if (py_body) {
                    // Check if route has BODY parameter defined
                    bool has_body_param = false;
                    std::string body_param_name = "body";
                    for (const auto& param_info : metadata->parameters) {
                        if (param_info.location == fasterapi::http::BODY) {
                            has_body_param = true;
                            body_param_name = param_info.name;
                            break;
                        }
                    }

                    // Add body with appropriate name
                    if (has_body_param) {
                        PyDict_SetItemString(kwargs, body_param_name.c_str(), py_body);
                    } else {
                        // Default to "body" if no BODY parameter defined
                        PyDict_SetItemString(kwargs, "body", py_body);
                    }
                    Py_DECREF(py_body);
                } else {
                    PyErr_Clear();  // Clear any JSON parse errors
                }
                Py_DECREF(loads);
            }
            Py_DECREF(json_module);
        }
    }

    // Call handler with kwargs
    PyObject* empty_tuple = PyTuple_New(0);
    PyObject* py_result = PyObject_Call(callable, empty_tuple, kwargs);
    Py_DECREF(empty_tuple);
    Py_DECREF(kwargs);

    if (py_result == nullptr) {
        // Python exception occurred
        PyErr_Print();
        result.status_code = 500;
        result.content_type = "application/json";
        result.body = "{\"error\":\"Internal Server Error\"}";
    } else {
        // Handle different FastAPI-style return types:

        // 1. Tuple: (data, status_code) e.g., return {"error": "Not found"}, 404
        if (PyTuple_Check(py_result) && PyTuple_Size(py_result) == 2) {
            PyObject* data = PyTuple_GetItem(py_result, 0);
            PyObject* status = PyTuple_GetItem(py_result, 1);

            if (PyLong_Check(status)) {
                result.status_code = PyLong_AsLong(status);
            } else {
                result.status_code = 200;
            }

            // Convert data to JSON
            PyObject* json_module = PyImport_ImportModule("json");
            if (json_module) {
                PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
                if (dumps) {
                    PyObject* json_str = PyObject_CallFunctionObjArgs(dumps, data, nullptr);
                    if (json_str) {
                        const char* json_cstr = PyUnicode_AsUTF8(json_str);
                        if (json_cstr) {
                            result.body = json_cstr;
                            result.content_type = "application/json";
                        }
                        Py_DECREF(json_str);
                    }
                    Py_DECREF(dumps);
                }
                Py_DECREF(json_module);
            }
        }
        // 2. Dict or List: Convert to JSON with 200 status
        else if (PyDict_Check(py_result) || PyList_Check(py_result)) {
            result.status_code = 200;
            result.content_type = "application/json";

            // Convert to JSON using Python's json.dumps()
            PyObject* json_module = PyImport_ImportModule("json");
            if (json_module) {
                PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
                if (dumps) {
                    PyObject* json_str = PyObject_CallFunctionObjArgs(dumps, py_result, nullptr);
                    if (json_str) {
                        const char* json_cstr = PyUnicode_AsUTF8(json_str);
                        if (json_cstr) {
                            result.body = json_cstr;
                        }
                        Py_DECREF(json_str);
                    }
                    Py_DECREF(dumps);
                }
                Py_DECREF(json_module);
            }
        }
        // 3. String: Return as-is
        else if (PyUnicode_Check(py_result)) {
            result.status_code = 200;
            result.content_type = "text/plain";
            const char* str = PyUnicode_AsUTF8(py_result);
            if (str) {
                result.body = str;
            }
        }
        // 4. None: Return empty 204 No Content
        else if (py_result == Py_None) {
            result.status_code = 204;
            result.content_type = "text/plain";
            result.body = "";
        }
        // 5. Other types: Try to convert to string
        else {
            result.status_code = 200;
            result.content_type = "text/plain";
            PyObject* str_obj = PyObject_Str(py_result);
            if (str_obj) {
                const char* str = PyUnicode_AsUTF8(str_obj);
                if (str) {
                    result.body = str;
                }
                Py_DECREF(str_obj);
            }
        }

        Py_DECREF(py_result);
    }

    // Validate response if schema is defined
    if (metadata && !metadata->response_schema.empty() && result.status_code >= 200 && result.status_code < 300) {
        auto schema = fasterapi::http::SchemaRegistry::instance().get_schema(metadata->response_schema);
        if (schema && !result.body.empty()) {
            fasterapi::http::ValidationResult validation_result = schema->validate_json(result.body);
            if (!validation_result.valid) {
                // Log warning but don't fail the response
                // (response was already generated by user code)
                LOG_WARN("PythonCallback",
                        "Response validation failed for schema '%s': %zu errors",
                        metadata->response_schema.c_str(),
                        validation_result.errors.size());
                for (const auto& error : validation_result.errors) {
                    // Build location string from vector
                    std::string location_str = "[";
                    for (size_t i = 0; i < error.loc.size(); ++i) {
                        if (i > 0) location_str += ", ";
                        location_str += error.loc[i];
                    }
                    location_str += "]";

                    LOG_WARN("PythonCallback", "  - %s: %s at %s",
                            error.type.c_str(),
                            error.msg.c_str(),
                            location_str.c_str());
                }
            } else {
                LOG_DEBUG("PythonCallback", "Response validation passed for schema: %s",
                         metadata->response_schema.c_str());
            }
        }
    }

    // Release GIL
    PyGILState_Release(gstate);

    return result;
}

fasterapi::core::future<fasterapi::core::result<PythonCallbackBridge::HandlerResult>>
PythonCallbackBridge::invoke_handler_async(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body
) noexcept {
    using namespace fasterapi::core;
    using namespace fasterapi::python;

    // ProcessPoolExecutor is always ready after initialization (no check needed)
    LOG_DEBUG("PythonCallback", "Using ProcessPoolExecutor for handler execution");

    // Split path from query string for route matching
    std::string route_path = path;
    size_t query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        route_path = path.substr(0, query_pos);
    }

    // Use RouteRegistry to match the route pattern (handles parametrized routes)
    std::string path_pattern;
    const fasterapi::http::RouteMetadata* route_meta = nullptr;  // Declare outside if block for parameter extraction

    if (route_registry_) {
        route_meta = route_registry_->match(method, route_path);
        if (route_meta) {
            path_pattern = route_meta->path_pattern;
        }
    }

    // If not found in route_registry_, check internal metadata registry
    if (!route_meta) {
        // Try to match against internal metadata patterns
        for (const auto& [key, meta] : internal_route_metadata_) {
            // key format is "METHOD:pattern"
            size_t colon_pos = key.find(':');
            if (colon_pos != std::string::npos) {
                std::string meta_method = key.substr(0, colon_pos);
                std::string meta_pattern = key.substr(colon_pos + 1);

                if (meta_method == method && meta.compiled_pattern.matches(route_path)) {
                    route_meta = &meta;
                    path_pattern = meta_pattern;
                    break;
                }
            }
        }
    }

    // If still not found, fallback to exact path match
    if (!route_meta) {
        path_pattern = route_path;
    }

    // Lookup handler metadata using the PATTERN path (e.g., /user/{user_id})
    std::string route_key = method + ":" + path_pattern;
    LOG_WARN("PythonCallback", "INVOKE: Looking for route_key=%s (path_pattern=%s)", route_key.c_str(), path_pattern.c_str());
    auto meta_it = handler_metadata_.find(route_key);

    if (meta_it == handler_metadata_.end()) {
        // Handler metadata not found - return 404
        LOG_ERROR("PythonCallback", "INVOKE: Handler metadata NOT FOUND for route_key=%s", route_key.c_str());
        HandlerResult error_result;
        error_result.status_code = 404;
        error_result.content_type = "application/json";
        error_result.body = "{\"error\":\"Not Found\"}";
        return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
    }

    const HandlerMetadata& metadata = meta_it->second;
    LOG_WARN("PythonCallback", "INVOKE: Found metadata - module=%s, func=%s", metadata.module_name.c_str(), metadata.function_name.c_str());

    // Build Python args/kwargs in main interpreter (requires GIL)
    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* kwargs = PyDict_New();
    if (!kwargs) {
        PyGILState_Release(gstate);
        HandlerResult error_result;
        error_result.status_code = 500;
        error_result.content_type = "application/json";
        error_result.body = "{\"error\":\"Failed to create kwargs dict\"}";
        return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
    }

    // Extract and add parameters if route metadata is available
    if (route_meta) {
        LOG_DEBUG("PythonCallback", "Extracting params from URL: %s", path.c_str());

        // 1. Extract path parameters using CompiledRoutePattern
        auto path_params = route_meta->compiled_pattern.extract(route_path);
        LOG_DEBUG("PythonCallback", "Extracted %zu path params", path_params.size());

        // 2. Extract query parameters from URL
        auto query_params = fasterapi::http::ParameterExtractor::get_query_params(path);
        LOG_DEBUG("PythonCallback", "Extracted %zu query params", query_params.size());

        // 3. Parse JSON body once for BODY parameter extraction
        PyObject* parsed_body = nullptr;
        if (!body.empty()) {
            PyObject* json_module = PyImport_ImportModule("json");
            if (json_module) {
                PyObject* loads = PyObject_GetAttrString(json_module, "loads");
                if (loads) {
                    parsed_body = PyObject_CallFunction(loads, "s", body.c_str());
                    if (!parsed_body) {
                        PyErr_Clear();  // Clear JSON parse error
                        LOG_DEBUG("PythonCallback", "Failed to parse JSON body");
                    }
                    Py_DECREF(loads);
                }
                Py_DECREF(json_module);
            }
        }

        // 4. Build kwargs from parameters metadata
        for (const auto& param_info : route_meta->parameters) {
            PyObject* py_value = nullptr;

            if (param_info.location == fasterapi::http::PATH) {
                // Look up in path parameters
                auto it = path_params.find(param_info.name);
                if (it != path_params.end()) {
                    py_value = convert_to_python_type(it->second, param_info.type);
                }
            } else if (param_info.location == fasterapi::http::QUERY) {
                // Look up in query parameters
                auto it = query_params.find(param_info.name);
                if (it != query_params.end()) {
                    py_value = convert_to_python_type(it->second, param_info.type);
                } else if (!param_info.default_value.empty()) {
                    // Use default value if parameter not found
                    py_value = convert_to_python_type(param_info.default_value, param_info.type);
                } else if (!param_info.required) {
                    // Optional parameter not provided - use None
                    Py_INCREF(Py_None);
                    py_value = Py_None;
                }
            } else if (param_info.location == fasterapi::http::BODY) {
                // Extract individual field from parsed JSON body
                if (parsed_body && PyDict_Check(parsed_body)) {
                    PyObject* field_value = PyDict_GetItemString(parsed_body, param_info.name.c_str());
                    if (field_value) {
                        Py_INCREF(field_value);
                        py_value = field_value;
                    } else if (!param_info.default_value.empty()) {
                        // Use default value if field not found
                        py_value = convert_to_python_type(param_info.default_value, param_info.type);
                    } else if (!param_info.required) {
                        // Optional parameter not provided - use None
                        Py_INCREF(Py_None);
                        py_value = Py_None;
                    }
                }
            }

            // Add to kwargs if we got a value
            if (py_value) {
                PyDict_SetItemString(kwargs, param_info.name.c_str(), py_value);
                Py_DECREF(py_value);  // Dict took a reference
            }
        }

        // Cleanup parsed body
        Py_XDECREF(parsed_body);
    }

    PyObject* empty_args = PyTuple_New(0);

    PyGILState_Release(gstate);

    LOG_DEBUG("PythonCallback", "Submitting handler via metadata: module=%s, func=%s",
             metadata.module_name.c_str(), metadata.function_name.c_str());

    // Special case: __main__ module handlers must execute in main process
    // (ZMQ workers cannot import from parent's __main__)
    if (metadata.module_name == "__main__") {
        LOG_WARN("PythonCallback", "Executing __main__ handler: module=%s, func=%s", metadata.module_name.c_str(), metadata.function_name.c_str());

        // Re-acquire GIL for Python execution
        PyGILState_STATE exec_gstate = PyGILState_Ensure();

        // Get handler from registry using existing route_key (METHOD:PATTERN)
        // route_key was already set correctly earlier (line 629)
        auto handler_it = handlers_.find(route_key);
        if (handler_it == handlers_.end()) {
            PyGILState_Release(exec_gstate);
            LOG_ERROR("PythonCallback", "Handler not found for route: %s", route_key.c_str());
            HandlerResult error_result;
            error_result.status_code = 500;
            error_result.content_type = "application/json";
            error_result.body = "{\"error\":\"Handler not found\"}";
            return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
        }

        // handlers_ value is std::pair<int, PyObject*>, get the PyObject*
        PyObject* handler = handler_it->second.second;

        // Check if handler is async using inspect.iscoroutinefunction()
        PyObject* inspect_module = PyImport_ImportModule("inspect");
        if (!inspect_module) {
            PyErr_Clear();
            PyGILState_Release(exec_gstate);
            LOG_ERROR("PythonCallback", "Failed to import inspect module");
            HandlerResult error_result;
            error_result.status_code = 500;
            error_result.content_type = "application/json";
            error_result.body = "{\"error\":\"Failed to import inspect module\"}";
            return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
        }

        PyObject* iscoroutinefunction = PyObject_GetAttrString(inspect_module, "iscoroutinefunction");
        Py_DECREF(inspect_module);

        if (!iscoroutinefunction) {
            PyErr_Clear();
            PyGILState_Release(exec_gstate);
            LOG_ERROR("PythonCallback", "Failed to get inspect.iscoroutinefunction");
            HandlerResult error_result;
            error_result.status_code = 500;
            error_result.content_type = "application/json";
            error_result.body = "{\"error\":\"Failed to get inspect.iscoroutinefunction\"}";
            return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
        }

        PyObject* is_async_result = PyObject_CallFunctionObjArgs(iscoroutinefunction, handler, nullptr);
        Py_DECREF(iscoroutinefunction);

        bool is_async = false;
        if (is_async_result) {
            is_async = PyObject_IsTrue(is_async_result);
            Py_DECREF(is_async_result);
        } else {
            PyErr_Clear();
        }

        LOG_DEBUG("PythonCallback", "Handler is %s", is_async ? "async" : "sync");

        PyObject* result_obj = nullptr;

        if (is_async) {
            // Async handler: call it to get coroutine, then use asyncio.run() to execute
            PyObject* coro = PyObject_Call(handler, empty_args, kwargs);

            if (!coro) {
                PyErr_Print();
                PyGILState_Release(exec_gstate);
                LOG_ERROR("PythonCallback", "Failed to call async handler");
                HandlerResult error_result;
                error_result.status_code = 500;
                error_result.content_type = "application/json";
                error_result.body = "{\"error\":\"Failed to call async handler\"}";
                return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
            }

            // Import asyncio and get asyncio.run()
            PyObject* asyncio_module = PyImport_ImportModule("asyncio");
            if (!asyncio_module) {
                Py_DECREF(coro);
                PyErr_Print();
                PyGILState_Release(exec_gstate);
                LOG_ERROR("PythonCallback", "Failed to import asyncio module");
                HandlerResult error_result;
                error_result.status_code = 500;
                error_result.content_type = "application/json";
                error_result.body = "{\"error\":\"Failed to import asyncio module\"}";
                return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
            }

            PyObject* asyncio_run = PyObject_GetAttrString(asyncio_module, "run");
            Py_DECREF(asyncio_module);

            if (!asyncio_run) {
                Py_DECREF(coro);
                PyErr_Print();
                PyGILState_Release(exec_gstate);
                LOG_ERROR("PythonCallback", "Failed to get asyncio.run");
                HandlerResult error_result;
                error_result.status_code = 500;
                error_result.content_type = "application/json";
                error_result.body = "{\"error\":\"Failed to get asyncio.run\"}";
                return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
            }

            // Execute: asyncio.run(coro)
            result_obj = PyObject_CallFunctionObjArgs(asyncio_run, coro, nullptr);
            Py_DECREF(asyncio_run);
            Py_DECREF(coro);

            if (!result_obj) {
                PyErr_Print();
                PyGILState_Release(exec_gstate);
                LOG_ERROR("PythonCallback", "asyncio.run() failed");
                HandlerResult error_result;
                error_result.status_code = 500;
                error_result.content_type = "application/json";
                error_result.body = "{\"error\":\"asyncio.run() failed\"}";
                return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
            }
        } else {
            // Sync handler: call directly
            result_obj = PyObject_Call(handler, empty_args, kwargs);

            if (!result_obj) {
                PyErr_Print();
                PyGILState_Release(exec_gstate);
                LOG_ERROR("PythonCallback", "Failed to call sync handler");
                HandlerResult error_result;
                error_result.status_code = 500;
                error_result.content_type = "application/json";
                error_result.body = "{\"error\":\"Failed to call sync handler\"}";
                return future<result<HandlerResult>>::make_ready(ok(std::move(error_result)));
            }
        }

        // Convert result to HandlerResult
        HandlerResult final_result;

        if (PyDict_Check(result_obj)) {
            // Dict response - serialize to JSON
            PyObject* json_module = PyImport_ImportModule("json");
            if (json_module) {
                PyObject* dumps_func = PyObject_GetAttrString(json_module, "dumps");
                if (dumps_func) {
                    PyObject* json_str = PyObject_CallFunctionObjArgs(dumps_func, result_obj, nullptr);
                    if (json_str) {
                        const char* json_cstr = PyUnicode_AsUTF8(json_str);
                        if (json_cstr) {
                            final_result.status_code = 200;
                            final_result.content_type = "application/json";
                            final_result.body = json_cstr;
                        }
                        Py_DECREF(json_str);
                    } else {
                        PyErr_Clear();
                    }
                    Py_DECREF(dumps_func);
                }
                Py_DECREF(json_module);
            }
        } else if (PyUnicode_Check(result_obj)) {
            // String response
            const char* str = PyUnicode_AsUTF8(result_obj);
            if (str) {
                final_result.status_code = 200;
                final_result.content_type = "text/plain";
                final_result.body = str;
            }
        }

        Py_DECREF(result_obj);
        Py_DECREF(empty_args);
        Py_DECREF(kwargs);
        PyGILState_Release(exec_gstate);

        return future<result<HandlerResult>>::make_ready(ok(std::move(final_result)));
    }

    // Non-__main__ handlers: route through ProcessPoolExecutor
    // ZMQ workers can import these from their modules
    auto py_future = ProcessPoolExecutor::submit_with_metadata(
        metadata.module_name, metadata.function_name, empty_args, kwargs);

    // Transform future<result<PyObject*>> → future<result<HandlerResult>>
    // This conversion happens asynchronously when the Python handler completes
    return py_future.then([method, path, headers, body](result<PyObject*> py_result) -> result<HandlerResult> {
        if (!py_result.is_ok()) {
            LOG_ERROR("PythonCallback", "Handler execution failed in worker process");
            HandlerResult error_result;
            error_result.status_code = 500;
            error_result.content_type = "application/json";
            error_result.body = "{\"error\":\"Handler execution failed\"}";
            return ok(std::move(error_result));
        }

        PyObject* py_response = py_result.value();
        if (!py_response) {
            HandlerResult error_result;
            error_result.status_code = 500;
            error_result.content_type = "application/json";
            error_result.body = "{\"error\":\"Handler returned null\"}";
            return ok(std::move(error_result));
        }

        // Convert PyObject* to HandlerResult (requires GIL)
        PyGILState_STATE gstate = PyGILState_Ensure();

        HandlerResult result;

        // Extract status code, content type, and body from Python response
        // For now, use simplified extraction - full implementation will handle
        // Response objects, dicts, strings, etc.

        if (PyDict_Check(py_response)) {
            // Dict response - serialize to JSON
            PyObject* json_module = PyImport_ImportModule("json");
            if (json_module) {
                PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
                if (dumps) {
                    PyObject* json_str = PyObject_CallFunctionObjArgs(dumps, py_response, nullptr);
                    if (json_str) {
                        const char* json_cstr = PyUnicode_AsUTF8(json_str);
                        if (json_cstr) {
                            result.body = json_cstr;
                            result.content_type = "application/json";
                            result.status_code = 200;
                        }
                        Py_DECREF(json_str);
                    }
                    Py_DECREF(dumps);
                }
                Py_DECREF(json_module);
            }
        } else if (PyUnicode_Check(py_response)) {
            // String response
            const char* str = PyUnicode_AsUTF8(py_response);
            if (str) {
                result.body = str;
                result.content_type = "text/plain";
                result.status_code = 200;
            }
        } else {
            // Unknown type - convert to string
            PyObject* str_obj = PyObject_Str(py_response);
            if (str_obj) {
                const char* str = PyUnicode_AsUTF8(str_obj);
                if (str) {
                    result.body = str;
                    result.content_type = "text/plain";
                    result.status_code = 200;
                }
                Py_DECREF(str_obj);
            }
        }

        Py_DECREF(py_response);
        PyGILState_Release(gstate);

        return ok(std::move(result));
    });
}

void PythonCallbackBridge::cleanup() {
    // Acquire GIL for cleanup
    PyGILState_STATE gstate = PyGILState_Ensure();

    // Decrement reference counts
    for (auto& [key, pair] : handlers_) {
        Py_DECREF(pair.second);
    }

    handlers_.clear();

    PyGILState_Release(gstate);
}
