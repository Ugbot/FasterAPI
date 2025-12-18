#include "exception_handler.h"
#include "../core/logger.h"
#include <cstring>

namespace fasterapi {
namespace http {

// Constructor
ExceptionHandlerRegistry::ExceptionHandlerRegistry() {
    // Register built-in exception type handlers are done via defaults
}

// Destructor - release all Python references
ExceptionHandlerRegistry::~ExceptionHandlerRegistry() {
    // Note: This runs at program exit, GIL state is uncertain
    // Don't try to acquire GIL or decref here
    handlers_.clear();
    class_handlers_.clear();
}

void ExceptionHandlerRegistry::register_handler(const std::string& exc_type,
                                                 PyObject* handler,
                                                 bool is_async) {
    if (!handler || !PyCallable_Check(handler)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Release old handler if replacing
    auto it = handlers_.find(exc_type);
    if (it != handlers_.end() && it->second.handler) {
        Py_DECREF(it->second.handler);
    }

    // Store new handler (take ownership)
    Py_INCREF(handler);
    handlers_[exc_type] = PythonHandler{handler, is_async};

    LOG_DEBUG("ExcHandler", "Registered handler for exception type: %s (async=%d)",
              exc_type.c_str(), is_async ? 1 : 0);
}

void ExceptionHandlerRegistry::register_handler_for_class(PyObject* exc_class,
                                                           PyObject* handler,
                                                           bool is_async) {
    if (!exc_class || !handler || !PyCallable_Check(handler)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Release old handler if replacing
    auto it = class_handlers_.find(exc_class);
    if (it != class_handlers_.end() && it->second.handler) {
        Py_DECREF(it->second.handler);
    }

    // Store new handler (take ownership of handler ref, not class)
    Py_INCREF(handler);
    class_handlers_[exc_class] = PythonHandler{handler, is_async};

    // Also register by qualified name for string-based lookup
    PyObject* module = PyObject_GetAttrString(exc_class, "__module__");
    PyObject* qualname = PyObject_GetAttrString(exc_class, "__qualname__");

    if (module && qualname) {
        const char* mod_str = PyUnicode_AsUTF8(module);
        const char* name_str = PyUnicode_AsUTF8(qualname);
        if (mod_str && name_str) {
            std::string full_name = std::string(mod_str) + "." + name_str;
            Py_INCREF(handler);
            handlers_[full_name] = PythonHandler{handler, is_async};
            LOG_DEBUG("ExcHandler", "Registered handler for class: %s (async=%d)",
                      full_name.c_str(), is_async ? 1 : 0);
        }
    }

    Py_XDECREF(module);
    Py_XDECREF(qualname);
}

bool ExceptionHandlerRegistry::has_handler(const std::string& exc_type) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.find(exc_type) != handlers_.end();
}

void ExceptionHandlerRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    // GIL must be held to decref
    for (auto& [name, ph] : handlers_) {
        if (ph.handler) {
            Py_DECREF(ph.handler);
            ph.handler = nullptr;
        }
    }
    handlers_.clear();

    for (auto& [cls, ph] : class_handlers_) {
        if (ph.handler) {
            Py_DECREF(ph.handler);
            ph.handler = nullptr;
        }
    }
    class_handlers_.clear();
}

ExceptionInfo ExceptionHandlerRegistry::extract_exception_info() noexcept {
    ExceptionInfo info;

    // Fetch current Python exception
    PyObject* exc_type = nullptr;
    PyObject* exc_value = nullptr;
    PyObject* exc_tb = nullptr;
    PyErr_Fetch(&exc_type, &exc_value, &exc_tb);

    if (!exc_type) {
        // No exception
        info.type = ExceptionInfo::Type::UNKNOWN;
        info.detail = "Unknown error";
        return info;
    }

    // Normalize the exception
    PyErr_NormalizeException(&exc_type, &exc_value, &exc_tb);

    info.exc_type = exc_type;
    info.exc_value = exc_value;
    info.exc_traceback = exc_tb;

    // Get exception type name
    PyObject* type_name = PyObject_GetAttrString(exc_type, "__name__");
    if (type_name) {
        const char* name_str = PyUnicode_AsUTF8(type_name);
        if (name_str) {
            info.type_name = name_str;
        }
        Py_DECREF(type_name);
    }

    // Get qualified name (module.classname)
    PyObject* module = PyObject_GetAttrString(exc_type, "__module__");
    if (module) {
        const char* mod_str = PyUnicode_AsUTF8(module);
        if (mod_str && !info.type_name.empty()) {
            info.qualified_name = std::string(mod_str) + "." + info.type_name;
        }
        Py_DECREF(module);
    }

    // Determine exception type category
    if (info.type_name == "HTTPException" ||
        info.qualified_name.find("HTTPException") != std::string::npos ||
        info.qualified_name.find("starlette.exceptions") != std::string::npos) {
        info.type = ExceptionInfo::Type::HTTP_EXCEPTION;

        // Extract status_code attribute
        if (exc_value) {
            PyObject* status = PyObject_GetAttrString(exc_value, "status_code");
            if (status && PyLong_Check(status)) {
                info.status_code = static_cast<int>(PyLong_AsLong(status));
            }
            Py_XDECREF(status);

            // Extract detail attribute
            PyObject* detail = PyObject_GetAttrString(exc_value, "detail");
            if (detail) {
                if (PyUnicode_Check(detail)) {
                    const char* detail_str = PyUnicode_AsUTF8(detail);
                    if (detail_str) {
                        info.detail = detail_str;
                    }
                } else {
                    // Convert to string
                    PyObject* str_obj = PyObject_Str(detail);
                    if (str_obj) {
                        const char* str = PyUnicode_AsUTF8(str_obj);
                        if (str) {
                            info.detail = str;
                        }
                        Py_DECREF(str_obj);
                    }
                }
                Py_DECREF(detail);
            }

            // Extract headers attribute
            PyObject* headers = PyObject_GetAttrString(exc_value, "headers");
            if (headers && PyDict_Check(headers)) {
                PyObject* key;
                PyObject* value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(headers, &pos, &key, &value)) {
                    const char* k = PyUnicode_AsUTF8(key);
                    const char* v = PyUnicode_AsUTF8(value);
                    if (k && v) {
                        info.headers[k] = v;
                    }
                }
            }
            Py_XDECREF(headers);
        }
    } else if (info.type_name == "ValidationError" ||
               info.type_name == "RequestValidationError" ||
               info.qualified_name.find("ValidationError") != std::string::npos) {
        info.type = ExceptionInfo::Type::VALIDATION_ERROR;
        info.status_code = 422;

        // Extract validation errors
        if (exc_value) {
            // Try to get errors() method result
            PyObject* errors_method = PyObject_GetAttrString(exc_value, "errors");
            if (errors_method && PyCallable_Check(errors_method)) {
                PyObject* errors_result = PyObject_CallObject(errors_method, nullptr);
                if (errors_result) {
                    // Convert to JSON string for detail
                    PyObject* json_module = PyImport_ImportModule("json");
                    if (json_module) {
                        PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
                        if (dumps) {
                            PyObject* json_str = PyObject_CallFunctionObjArgs(dumps, errors_result, nullptr);
                            if (json_str) {
                                const char* str = PyUnicode_AsUTF8(json_str);
                                if (str) {
                                    info.detail = str;
                                }
                                Py_DECREF(json_str);
                            }
                            Py_DECREF(dumps);
                        }
                        Py_DECREF(json_module);
                    }
                    Py_DECREF(errors_result);
                }
            }
            Py_XDECREF(errors_method);
        }
    } else {
        info.type = ExceptionInfo::Type::PYTHON_EXCEPTION;
        info.status_code = 500;

        // Get string representation as detail
        if (exc_value) {
            PyObject* str_obj = PyObject_Str(exc_value);
            if (str_obj) {
                const char* str = PyUnicode_AsUTF8(str_obj);
                if (str) {
                    info.detail = str;
                }
                Py_DECREF(str_obj);
            }
        }
    }

    // Clean up exception references (caller doesn't own them)
    Py_XDECREF(exc_type);
    Py_XDECREF(exc_value);
    Py_XDECREF(exc_tb);

    // Clear stored references since we released them
    info.exc_type = nullptr;
    info.exc_value = nullptr;
    info.exc_traceback = nullptr;

    LOG_DEBUG("ExcHandler", "Extracted exception: type=%s qualified=%s status=%d detail=%s",
              info.type_name.c_str(), info.qualified_name.c_str(),
              info.status_code, info.detail.c_str());

    return info;
}

const ExceptionHandlerRegistry::PythonHandler*
ExceptionHandlerRegistry::find_handler(const ExceptionInfo& info) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    // Try qualified name first (most specific)
    if (!info.qualified_name.empty()) {
        auto it = handlers_.find(info.qualified_name);
        if (it != handlers_.end()) {
            return &it->second;
        }
    }

    // Try simple type name
    if (!info.type_name.empty()) {
        auto it = handlers_.find(info.type_name);
        if (it != handlers_.end()) {
            return &it->second;
        }
    }

    // Try generic "Exception" handler
    {
        auto it = handlers_.find("Exception");
        if (it != handlers_.end()) {
            return &it->second;
        }
    }

    return nullptr;
}

ExceptionResponse ExceptionHandlerRegistry::default_http_exception_handler(const ExceptionInfo& info) {
    return ExceptionResponse::from_http_exception(info.status_code, info.detail, info.headers);
}

ExceptionResponse ExceptionHandlerRegistry::default_validation_error_handler(const ExceptionInfo& info) {
    ExceptionResponse resp;
    resp.status_code = 422;
    resp.content_type = "application/json";

    // If detail is already JSON (from errors()), wrap it
    if (!info.detail.empty() && (info.detail[0] == '[' || info.detail[0] == '{')) {
        resp.body = "{\"detail\":" + info.detail + "}";
    } else {
        resp.body = R"({"detail":"Validation Error"})";
    }

    return resp;
}

ExceptionResponse ExceptionHandlerRegistry::default_exception_handler(const ExceptionInfo& info) {
    // Don't expose internal error details
    return ExceptionResponse::internal_error();
}

ExceptionResponse ExceptionHandlerRegistry::invoke_python_handler(
    const PythonHandler& handler,
    const ExceptionInfo& info) noexcept {

    // Create Request and Exception objects for the handler
    // Handler signature: async def handler(request: Request, exc: Exception) -> Response

    // For now, we'll create minimal objects
    // Full implementation would reconstruct Request from stored data

    PyObject* fasterapi_module = PyImport_ImportModule("fasterapi.http.request");
    PyObject* request_obj = nullptr;

    if (fasterapi_module) {
        PyObject* request_class = PyObject_GetAttrString(fasterapi_module, "Request");
        if (request_class) {
            // Create minimal Request object
            // In full implementation, we'd pass actual request data
            PyObject* scope = PyDict_New();
            PyDict_SetItemString(scope, "type", PyUnicode_FromString("http"));
            PyDict_SetItemString(scope, "method", PyUnicode_FromString("GET"));
            PyDict_SetItemString(scope, "path", PyUnicode_FromString("/"));

            request_obj = PyObject_CallFunctionObjArgs(request_class, scope, nullptr);
            Py_DECREF(scope);
            Py_DECREF(request_class);
        }
        Py_DECREF(fasterapi_module);
    }

    if (!request_obj) {
        // Fallback: use None as request
        Py_INCREF(Py_None);
        request_obj = Py_None;
    }

    // Create exception object
    PyObject* exc_obj = nullptr;

    // Try to create HTTPException for HTTP exceptions
    if (info.type == ExceptionInfo::Type::HTTP_EXCEPTION) {
        PyObject* exc_module = PyImport_ImportModule("fasterapi.exceptions");
        if (exc_module) {
            PyObject* http_exc_class = PyObject_GetAttrString(exc_module, "HTTPException");
            if (http_exc_class) {
                PyObject* kwargs = PyDict_New();
                PyDict_SetItemString(kwargs, "status_code", PyLong_FromLong(info.status_code));
                if (!info.detail.empty()) {
                    PyDict_SetItemString(kwargs, "detail", PyUnicode_FromString(info.detail.c_str()));
                }
                PyObject* args = PyTuple_New(0);
                exc_obj = PyObject_Call(http_exc_class, args, kwargs);
                Py_DECREF(args);
                Py_DECREF(kwargs);
                Py_DECREF(http_exc_class);
            }
            Py_DECREF(exc_module);
        }
    }

    if (!exc_obj) {
        // Create generic Exception
        PyObject* exc_class = PyExc_Exception;
        exc_obj = PyObject_CallFunction(exc_class, "s", info.detail.c_str());
        if (!exc_obj) {
            PyErr_Clear();
            Py_INCREF(Py_None);
            exc_obj = Py_None;
        }
    }

    // Call the handler
    PyObject* result = nullptr;

    if (handler.is_async) {
        // For async handlers, we need to run in event loop
        // This is simplified - full implementation would integrate with asyncio

        PyObject* asyncio = PyImport_ImportModule("asyncio");
        if (asyncio) {
            PyObject* coro = PyObject_CallFunctionObjArgs(handler.handler, request_obj, exc_obj, nullptr);
            if (coro) {
                // Try to get running loop or create new one
                PyObject* get_loop = PyObject_GetAttrString(asyncio, "get_event_loop");
                if (get_loop) {
                    PyObject* loop = PyObject_CallObject(get_loop, nullptr);
                    if (loop && !PyErr_Occurred()) {
                        PyObject* run_until = PyObject_GetAttrString(loop, "run_until_complete");
                        if (run_until) {
                            result = PyObject_CallFunctionObjArgs(run_until, coro, nullptr);
                            Py_DECREF(run_until);
                        }
                        Py_DECREF(loop);
                    } else {
                        PyErr_Clear();
                        // No running loop - use asyncio.run()
                        PyObject* run = PyObject_GetAttrString(asyncio, "run");
                        if (run) {
                            result = PyObject_CallFunctionObjArgs(run, coro, nullptr);
                            Py_DECREF(run);
                        }
                    }
                    Py_DECREF(get_loop);
                }
                Py_DECREF(coro);
            }
            Py_DECREF(asyncio);
        }
    } else {
        // Sync handler - call directly
        result = PyObject_CallFunctionObjArgs(handler.handler, request_obj, exc_obj, nullptr);
    }

    Py_DECREF(request_obj);
    Py_DECREF(exc_obj);

    ExceptionResponse resp;

    if (!result || PyErr_Occurred()) {
        PyErr_Clear();
        // Handler failed - use default response
        resp = ExceptionResponse::internal_error();
    } else {
        // Extract response from result
        // Result should be a Response object or dict

        if (PyDict_Check(result)) {
            // Dict response - convert to JSON
            resp.status_code = 200;
            resp.content_type = "application/json";

            PyObject* json_module = PyImport_ImportModule("json");
            if (json_module) {
                PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
                if (dumps) {
                    PyObject* json_str = PyObject_CallFunctionObjArgs(dumps, result, nullptr);
                    if (json_str) {
                        const char* str = PyUnicode_AsUTF8(json_str);
                        if (str) {
                            resp.body = str;
                        }
                        Py_DECREF(json_str);
                    }
                    Py_DECREF(dumps);
                }
                Py_DECREF(json_module);
            }
        } else {
            // Try to get Response attributes
            PyObject* status = PyObject_GetAttrString(result, "status_code");
            if (status && PyLong_Check(status)) {
                resp.status_code = static_cast<int>(PyLong_AsLong(status));
            }
            Py_XDECREF(status);

            // Try media_type
            PyObject* media_type = PyObject_GetAttrString(result, "media_type");
            if (media_type && PyUnicode_Check(media_type)) {
                const char* mt = PyUnicode_AsUTF8(media_type);
                if (mt) {
                    resp.content_type = mt;
                }
            }
            Py_XDECREF(media_type);

            // Try body
            PyObject* body = PyObject_GetAttrString(result, "body");
            if (body) {
                if (PyBytes_Check(body)) {
                    char* data;
                    Py_ssize_t len;
                    PyBytes_AsStringAndSize(body, &data, &len);
                    resp.body = std::string(data, len);
                } else if (PyUnicode_Check(body)) {
                    const char* str = PyUnicode_AsUTF8(body);
                    if (str) {
                        resp.body = str;
                    }
                }
            }
            Py_XDECREF(body);

            PyErr_Clear();
        }

        Py_DECREF(result);
    }

    return resp;
}

ExceptionResponse ExceptionHandlerRegistry::handle_exception_sync(const ExceptionInfo& info) noexcept {
    // Try to find registered handler
    const PythonHandler* handler = find_handler(info);

    if (handler) {
        return invoke_python_handler(*handler, info);
    }

    // Use default handlers based on exception type
    switch (info.type) {
        case ExceptionInfo::Type::HTTP_EXCEPTION:
            return default_http_exception_handler(info);

        case ExceptionInfo::Type::VALIDATION_ERROR:
        case ExceptionInfo::Type::REQUEST_VALIDATION:
            return default_validation_error_handler(info);

        case ExceptionInfo::Type::PYTHON_EXCEPTION:
        case ExceptionInfo::Type::UNKNOWN:
        default:
            return default_exception_handler(info);
    }
}

core::future<ExceptionResponse> ExceptionHandlerRegistry::handle_exception(const ExceptionInfo& info) noexcept {
    // For now, use sync handling wrapped in ready future
    // Full async implementation would use then() chains for async handlers

    ExceptionResponse resp = handle_exception_sync(info);
    return core::future<ExceptionResponse>::make_ready(std::move(resp));
}

} // namespace http
} // namespace fasterapi
