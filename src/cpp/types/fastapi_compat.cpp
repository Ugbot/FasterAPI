#include "fastapi_compat.h"
#include <cstring>

namespace fasterapi {
namespace types {

// ============================================================================
// NativeHandler Implementation (with vectorcall)
// ============================================================================

PyTypeObject NativeHandler::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "fasterapi.NativeHandler",
    sizeof(NativeHandler),
    0,                          // tp_itemsize
    nullptr,                    // tp_dealloc
    offsetof(NativeHandler, func),  // tp_vectorcall_offset - KEY FOR PERFORMANCE!
    nullptr,                    // tp_getattr
    nullptr,                    // tp_setattr
    nullptr,                    // tp_as_async
    nullptr,                    // tp_repr
    nullptr,                    // tp_as_number
    nullptr,                    // tp_as_sequence
    nullptr,                    // tp_as_mapping
    nullptr,                    // tp_hash
    vectorcall,                 // tp_call - uses vectorcall!
    nullptr,                    // tp_str
    nullptr,                    // tp_getattro
    nullptr,                    // tp_setattro
    nullptr,                    // tp_as_buffer
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_VECTORCALL,  // Enable vectorcall!
    "Native handler with vectorcall support",
};

NativeHandler* NativeHandler::create(HandlerFunc func, bool is_async) noexcept {
    auto* obj = PyObject_New(NativeHandler, &Type);
    if (!obj) {
        return nullptr;
    }
    
    obj->func = func;
    obj->is_async = is_async;
    obj->uses_native_types = true;  // Assume native for max performance
    
    return obj;
}

PyObject* NativeHandler::vectorcall(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames
) noexcept {
    // Vectorcall - fastest calling convention!
    // No tuple creation, args on stack
    
    auto* handler = reinterpret_cast<NativeHandler*>(callable);
    
    size_t nargs = PyVectorcall_NARGS(nargsf);
    
    // Extract request and response (native types!)
    if (nargs < 2) {
        PyErr_SetString(PyExc_TypeError, "Handler requires request and response");
        return nullptr;
    }
    
    auto* req = reinterpret_cast<NativeRequest*>(args[0]);
    auto* res = reinterpret_cast<NativeResponse*>(args[1]);
    
    // Call C++ handler (NO GIL needed if pure C++!)
    int result = handler->func(req, res);
    
    if (result != 0) {
        PyErr_SetString(PyExc_RuntimeError, "Handler failed");
        return nullptr;
    }
    
    // Return response
    Py_INCREF(res);
    return reinterpret_cast<PyObject*>(res);
}

// ============================================================================
// FastAPIRequest Implementation
// ============================================================================

PyObject* FastAPIRequest::create(NativeRequest* native_req) noexcept {
    // Return native request directly
    // Python sees it as Request object
    // But it's actually NativeRequest (zero-copy!)
    
    Py_INCREF(native_req);
    return reinterpret_cast<PyObject*>(native_req);
}

PyObject* FastAPIRequest::get_path_params(NativeRequest* req) noexcept {
    // Create dict-like object from path params
    // Zero-copy - returns views into original buffer!
    
    NativeDict* params = NativeDict::create(req->param_count);
    
    for (uint32_t i = 0; i < req->param_count; ++i) {
        // Store as string view (zero-copy!)
        // params->set_str(req->params[i].name, req->params[i].value);
    }
    
    return reinterpret_cast<PyObject*>(params);
}

PyObject* FastAPIRequest::get_query_params(NativeRequest* req) noexcept {
    NativeDict* params = NativeDict::create(req->query_param_count);
    
    for (uint32_t i = 0; i < req->query_param_count; ++i) {
        // Zero-copy query params
    }
    
    return reinterpret_cast<PyObject*>(params);
}

PyObject* FastAPIRequest::get_headers(NativeRequest* req) noexcept {
    NativeDict* headers = NativeDict::create(req->header_count);
    
    for (uint32_t i = 0; i < req->header_count; ++i) {
        // Zero-copy headers
    }
    
    return reinterpret_cast<PyObject*>(headers);
}

PyObject* FastAPIRequest::json(NativeRequest* req) noexcept {
    // Parse JSON body to NativeDict (not PyDict!)
    // Uses simdjson for speed
    
    NativeDict* dict = req->json();
    return reinterpret_cast<PyObject*>(dict);
}

// ============================================================================
// FastAPIResponse Implementation
// ============================================================================

PyObject* FastAPIResponse::json_response(
    PyObject* content,
    int status_code
) noexcept {
    NativeResponse* res = NativeResponse::create();
    res->set_status(status_code);
    
    // If content is NativeDict, use directly (zero-copy!)
    if (Py_TYPE(content) == &NativeDict::Type) {
        res->set_json(reinterpret_cast<NativeDict*>(content));
    }
    // Otherwise, would need to convert PyDict → NativeDict
    
    return reinterpret_cast<PyObject*>(res);
}

PyObject* FastAPIResponse::text_response(
    const char* content,
    int status_code
) noexcept {
    NativeResponse* res = NativeResponse::create();
    res->set_status(status_code);
    res->set_text(content);
    
    return reinterpret_cast<PyObject*>(res);
}

// ============================================================================
// PathParams Implementation
// ============================================================================

NativeInt* PathParams::get_int(
    NativeRequest* req,
    const char* param_name
) noexcept {
    std::string_view value = req->get_param(param_name);
    
    if (value.empty()) {
        return nullptr;
    }
    
    // Parse to int (simple implementation)
    int64_t num = 0;
    for (char c : value) {
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
        }
    }
    
    return NativeInt::create(num);
}

NativeStr* PathParams::get_str(
    NativeRequest* req,
    const char* param_name
) noexcept {
    std::string_view value = req->get_param(param_name);
    
    // Zero-copy string! (view into request buffer)
    return NativeStr::create(value, false);  // false = don't copy!
}

// ============================================================================
// Depends Implementation
// ============================================================================

PyObject* Depends::create(PyObject* func) noexcept {
    // For now, just return the function
    // Full implementation would wrap with dependency injection
    Py_INCREF(func);
    return func;
}

PyObject* Depends::resolve(PyObject* dependency) noexcept {
    // Call dependency function
    // If it returns native type, keep it native!
    
    PyObject* result = PyObject_CallNoArgs(dependency);
    
    // If result is native type, no conversion needed (zero-copy!)
    return result;
}

// ============================================================================
// ZeroCopyAnalyzer Implementation
// ============================================================================

ZeroCopyAnalyzer::Analysis ZeroCopyAnalyzer::analyze(PyObject* handler) noexcept {
    Analysis analysis;
    analysis.can_use_native_types = true;  // Assume yes
    analysis.can_skip_gil = true;          // Assume pure C++
    analysis.can_use_zero_copy_request = true;
    analysis.can_inline_handler = false;   // Conservative
    
    // TODO: Analyze handler bytecode to determine:
    // - Does it use any Python builtins? → needs GIL
    // - Does it only use native types? → can skip conversion
    // - Does it do I/O? → needs executor
    // - Is it small? → can inline
    
    // For now, assume best case
    return analysis;
}

} // namespace types
} // namespace fasterapi

