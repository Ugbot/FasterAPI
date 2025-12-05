#include "process_pool_executor.h"
#include "binary_kwargs.h"
#include "../core/logger.h"
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <cstdlib>

namespace fasterapi {

// Forward declaration - implemented in unified_server.cpp
namespace http {
    extern void signal_ws_response_ready();
}

namespace python {

using namespace fasterapi::core;

// Helper macros for dual-mode IPC
#ifdef FASTERAPI_USE_ZMQ
#define IPC_CALL(method, ...) \
    (zmq_ipc_ ? zmq_ipc_->method(__VA_ARGS__) : shm_ipc_->method(__VA_ARGS__))
#define IPC_CHECK() (zmq_ipc_ || shm_ipc_)
#else
#define IPC_CALL(method, ...) shm_ipc_->method(__VA_ARGS__)
#define IPC_CHECK() (shm_ipc_)
#endif

// Singleton instance
std::unique_ptr<ProcessPoolExecutor> ProcessPoolExecutor::instance_ = nullptr;
std::mutex ProcessPoolExecutor::instance_mutex_;

ProcessPoolExecutor::ProcessPoolExecutor(const Config& config)
    : config_(config)
    , shutdown_(false) {

    // Auto-detect number of workers
    if (config_.num_workers == 0) {
        config_.num_workers = std::thread::hardware_concurrency();
        if (config_.num_workers == 0) {
            config_.num_workers = 4;  // Fallback
        }
    }

    LOG_INFO("ProcessPoolExecutor", "Initializing with %u workers", config_.num_workers);

    // Generate unique IPC identifier
    ipc_id_ = "fasterapi_" + std::to_string(getpid());

    // Create appropriate IPC based on configuration
#ifdef FASTERAPI_USE_ZMQ
    if (config_.use_zeromq) {
        // ZeroMQ IPC (default, multi-language workers)
        LOG_INFO("ProcessPoolExecutor", "Using ZeroMQ IPC (default)");
        zmq_ipc_ = std::make_unique<ZmqIPC>(ipc_id_);
        LOG_INFO("ProcessPoolExecutor", "Created ZeroMQ IPC: %s", ipc_id_.c_str());
    } else
#endif
    {
        // Shared memory IPC (legacy/deprecated, unfinished implementation)
        LOG_INFO("ProcessPoolExecutor", "Using shared memory IPC (legacy/deprecated)");
        std::string shm_name = "/" + ipc_id_;  // POSIX requires leading slash
        shm_ipc_ = std::make_unique<SharedMemoryIPC>(shm_name);
        LOG_INFO("ProcessPoolExecutor", "Created shared memory: %s", shm_name.c_str());
    }

    // Initialize WebSocket response queue (lock-free, power-of-2 capacity)
    // 4096 should handle burst traffic without blocking
    ws_response_queue_ = std::make_unique<AeronSPSCQueue<WsResponse>>(4096);

    // Start worker processes
    start_workers();

    // Start response reader thread
    response_thread_ = std::thread(&ProcessPoolExecutor::response_reader_loop, this);

    LOG_INFO("ProcessPoolExecutor", "Initialization complete");
}

ProcessPoolExecutor::~ProcessPoolExecutor() {
    shutdown();
}

void ProcessPoolExecutor::start_workers() {
    // Get Python executable path
    const char* python_exe = config_.python_executable.c_str();

    // Get project directory
    std::string project_dir = config_.project_dir;
    if (project_dir.empty()) {
        project_dir = std::getenv("FASTERAPI_PROJECT_DIR")
                      ? std::getenv("FASTERAPI_PROJECT_DIR")
                      : ".";
    }

    LOG_INFO("ProcessPoolExecutor", "Starting %u workers with python: %s",
             config_.num_workers, python_exe);
    LOG_INFO("ProcessPoolExecutor", "Project directory: %s", project_dir.c_str());

    for (uint32_t i = 0; i < config_.num_workers; ++i) {
        pid_t pid = fork();

        if (pid < 0) {
            // Fork failed
            LOG_ERROR("ProcessPoolExecutor", "FATAL: Failed to fork worker %u: %s", i, strerror(errno));
            std::abort();
        }
        else if (pid == 0) {
            // Child process - execute Python worker

            // Set environment variables
            setenv("FASTERAPI_PROJECT_DIR", project_dir.c_str(), 1);

            // Build arguments for Python worker
            std::string worker_id_str = std::to_string(i);

            // Choose worker module based on IPC mode
#ifdef FASTERAPI_USE_ZMQ
            if (config_.use_zeromq) {
                // Execute: python3.13 -m fasterapi.core.zmq_worker <ipc_prefix> <worker_id>
                execlp(python_exe, python_exe,
                       "-m", "fasterapi.core.zmq_worker",
                       ipc_id_.c_str(),
                       worker_id_str.c_str(),
                       nullptr);
            } else
#endif
            {
                // Execute: python3.13 -m fasterapi.core.worker_pool <shm_name> <worker_id>
                std::string shm_name = "/" + ipc_id_;
                execlp(python_exe, python_exe,
                       "-m", "fasterapi.core.worker_pool",
                       shm_name.c_str(),
                       worker_id_str.c_str(),
                       nullptr);
            }

            // If execl returns, it failed
            std::cerr << "Failed to exec Python worker: " << strerror(errno) << std::endl;
            _exit(1);
        }
        else {
            // Parent process - save worker PID
            worker_pids_.push_back(pid);
            LOG_INFO("ProcessPoolExecutor", "Started worker %u (PID: %d)", i, pid);
        }
    }

    // Give workers a moment to start up
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void ProcessPoolExecutor::stop_workers() {
    if (worker_pids_.empty()) {
        return;
    }

    LOG_INFO("ProcessPoolExecutor", "Stopping %zu workers", worker_pids_.size());

    // Signal shutdown via IPC
    if (IPC_CHECK()) {
        IPC_CALL(signal_shutdown);
    }

    // Give workers time to exit gracefully
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Force kill any remaining workers
    for (pid_t pid : worker_pids_) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == 0) {
            // Process still running, kill it
            LOG_WARN("ProcessPoolExecutor", "Force killing worker PID %d", pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }

    worker_pids_.clear();
    LOG_INFO("ProcessPoolExecutor", "All workers stopped");
}

void ProcessPoolExecutor::response_reader_loop() {
    LOG_INFO("ProcessPoolExecutor", "Response reader thread started");

    while (!shutdown_.load(std::memory_order_acquire)) {
#ifdef FASTERAPI_USE_ZMQ
        if (zmq_ipc_) {
            // Use unified response reading for ZMQ
            MessageType msg_type;
            std::vector<uint8_t> raw_data;

            bool ok = zmq_ipc_->read_any_response(msg_type, raw_data);

            // CRITICAL: Check shutdown IMMEDIATELY after waking from semaphore
            if (shutdown_.load(std::memory_order_acquire)) {
                break;
            }

            if (!ok) {
                break;
            }

            // Route based on message type
            switch (msg_type) {
                case MessageType::RESPONSE: {
                    // HTTP response - deserialize and handle
                    uint32_t request_id;
                    uint16_t status_code;
                    bool success;
                    std::string body_json;
                    std::string error_message;

                    if (!zmq_ipc_->deserialize_response(raw_data, request_id, status_code, success, body_json, error_message)) {
                        LOG_ERROR("ProcessPoolExecutor", "Failed to deserialize HTTP response");
                        continue;
                    }

                    handle_http_response(request_id, status_code, success, body_json, error_message);
                    break;
                }

                case MessageType::WS_SEND:
                case MessageType::WS_CLOSE: {
                    // WebSocket response - parse and push to lock-free queue
                    uint64_t connection_id;
                    std::string payload;
                    bool is_binary;
                    uint16_t close_code;

                    if (!ZmqIPC::parse_ws_response(raw_data, connection_id, payload, is_binary, close_code)) {
                        LOG_ERROR("ProcessPoolExecutor", "Failed to parse WebSocket response");
                        continue;
                    }

                    LOG_DEBUG("ProcessPoolExecutor", "WS response: type=%d conn=%lu payload_len=%zu",
                             static_cast<int>(msg_type), connection_id, payload.size());

                    // Push to lock-free queue (no mutex!)
                    WsResponse ws_resp(msg_type, connection_id, std::move(payload), is_binary, close_code);
                    if (!ws_response_queue_->try_push(std::move(ws_resp))) {
                        LOG_WARN("ProcessPoolExecutor", "WebSocket response queue full, dropping message for conn=%lu", connection_id);
                    } else {
                        // Wake event loop to dispatch response
                        http::signal_ws_response_ready();
                    }
                    break;
                }

                default:
                    LOG_WARN("ProcessPoolExecutor", "Unexpected message type: %d", static_cast<int>(msg_type));
                    break;
            }
        } else
#endif
        {
            // Shared memory IPC path (legacy) - only handles HTTP responses
            uint32_t request_id;
            uint16_t status_code;
            bool success;
            std::string body_json;
            std::string error_message;

            bool ok = IPC_CALL(read_response, request_id, status_code, success, body_json, error_message);

            if (shutdown_.load(std::memory_order_acquire)) {
                break;
            }

            if (!ok) {
                break;
            }

            handle_http_response(request_id, status_code, success, body_json, error_message);
        }
    }

    LOG_INFO("ProcessPoolExecutor", "Response reader thread exiting");
}

void ProcessPoolExecutor::handle_http_response(uint32_t request_id, uint16_t status_code, bool success,
                                                const std::string& body_json, const std::string& error_message) {
    // Find pending request
    promise<result<PyObject*>> prom;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_requests_.find(request_id);
        if (it != pending_requests_.end()) {
            prom = std::move(it->second);
            pending_requests_.erase(it);
        } else {
            LOG_WARN("ProcessPoolExecutor", "Received response for unknown request ID: %u", request_id);
            return;
        }
    }

    // Deserialize and resolve promise
    if (success) {
        PyObject* result = deserialize_response(body_json);
        prom.set_value(fasterapi::core::ok(result));
        stats_.tasks_completed.fetch_add(1, std::memory_order_relaxed);
    } else {
        LOG_ERROR("ProcessPoolExecutor", "Request %u failed: %s", request_id, error_message.c_str());
        prom.set_value(fasterapi::core::err<PyObject*>(error_code::python_error));
        stats_.tasks_failed.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ProcessPoolExecutor::poll_ws_response(WsResponse& response) {
    return ws_response_queue_->try_pop(response);
}

bool ProcessPoolExecutor::has_ws_responses() const {
    return !ws_response_queue_->empty();
}

void ProcessPoolExecutor::shutdown() {
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) {
        return;  // Already shutdown
    }

    LOG_INFO("ProcessPoolExecutor", "Shutting down...");

    // Stop workers and signal IPC shutdown (sends shutdown messages to request queue)
    stop_workers();

    // Wake response reader thread (CRITICAL: must do this before joining)
    // The thread might be blocked on sem_wait() in read_response()
    // Posting to the semaphore wakes it, then it sees shutdown_ flag and exits
    if (IPC_CHECK()) {
        IPC_CALL(wake_response_reader);
    }

    // Now wait for response thread to exit cleanly
    // Thread will wake from semaphore, see shutdown_ flag, and exit its loop
    if (response_thread_.joinable()) {
        response_thread_.join();
    }

    // Clean up pending requests
    // SAFE: response_thread_ has fully exited after join(), so there's NO concurrent access
    // No mutex lock needed - the thread is guaranteed to have exited
    for (auto& [req_id, prom] : pending_requests_) {
        prom.set_value(fasterapi::core::err<PyObject*>(error_code::invalid_state));
    }
    pending_requests_.clear();

    LOG_INFO("ProcessPoolExecutor", "Shutdown complete. Stats: submitted=%llu, completed=%llu, failed=%llu",
             stats_.tasks_submitted.load(),
             stats_.tasks_completed.load(),
             stats_.tasks_failed.load());
}

future<result<PyObject*>> ProcessPoolExecutor::submit_with_metadata(
    const std::string& module_name,
    const std::string& function_name,
    PyObject* args,
    PyObject* kwargs
) {
    return instance().submit_with_metadata_impl(module_name, function_name, args, kwargs);
}

future<result<PyObject*>> ProcessPoolExecutor::submit_with_metadata_impl(
    const std::string& module_name,
    const std::string& function_name,
    PyObject* args,
    PyObject* kwargs
) {
    if (shutdown_.load(std::memory_order_acquire)) {
        promise<result<PyObject*>> prom;
        prom.set_value(fasterapi::core::err<PyObject*>(error_code::invalid_state));
        return prom.get_future();
    }

    // Generate request ID
    uint32_t request_id = generate_request_id();

    // Create promise for response
    promise<result<PyObject*>> prom;
    future<result<PyObject*>> fut = prom.get_future();

    // Store promise
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[request_id] = std::move(prom);
    }

    bool ok = false;

#ifdef FASTERAPI_USE_ZMQ
    if (zmq_ipc_) {
        // Binary TLV encoding for ZMQ path (~26x faster than JSON)
        PooledBuffer buffer;
        size_t encoded_size = 0;

        if (serialize_kwargs_binary(args, kwargs, buffer, encoded_size)) {
            // Use binary write path
            ok = zmq_ipc_->write_request_binary(
                request_id,
                module_name,
                function_name,
                buffer.data(),
                encoded_size,
                PayloadFormat::FORMAT_BINARY_TLV
            );
        } else {
            // Fallback to JSON if binary encoding fails
            std::string kwargs_json = serialize_kwargs(args, kwargs);
            ok = zmq_ipc_->write_request(request_id, module_name, function_name, kwargs_json,
                                         PayloadFormat::FORMAT_JSON);
        }
    } else
#endif
    {
        // Shared memory IPC path (legacy) - uses JSON
        std::string kwargs_json = serialize_kwargs(args, kwargs);
        ok = shm_ipc_->write_request(request_id, module_name, function_name, kwargs_json);
    }

    if (!ok) {
        LOG_ERROR("ProcessPoolExecutor", "Failed to write request to IPC");

        // Remove and fail promise
        promise<result<PyObject*>> failed_prom;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(request_id);
            if (it != pending_requests_.end()) {
                failed_prom = std::move(it->second);
                pending_requests_.erase(it);
            }
        }
        failed_prom.set_value(fasterapi::core::err<PyObject*>(error_code::internal_error));

        return failed_prom.get_future();
    }

    stats_.tasks_submitted.fetch_add(1, std::memory_order_relaxed);

    return fut;
}

uint32_t ProcessPoolExecutor::generate_request_id() {
    return next_request_id_.fetch_add(1, std::memory_order_relaxed);
}

std::string ProcessPoolExecutor::serialize_kwargs(PyObject* args, PyObject* kwargs) {
    // Build kwargs dict from args and kwargs
    PyObject* combined_kwargs = PyDict_New();

    if (!combined_kwargs) {
        return "{}";
    }

    // Merge kwargs if provided
    if (kwargs && PyDict_Check(kwargs)) {
        PyDict_Update(combined_kwargs, kwargs);
    }

    // Convert to JSON string
    PyObject* json_module = PyImport_ImportModule("json");
    if (!json_module) {
        Py_DECREF(combined_kwargs);
        PyErr_Clear();
        return "{}";
    }

    PyObject* dumps_func = PyObject_GetAttrString(json_module, "dumps");
    if (!dumps_func) {
        Py_DECREF(json_module);
        Py_DECREF(combined_kwargs);
        PyErr_Clear();
        return "{}";
    }

    PyObject* json_str = PyObject_CallFunctionObjArgs(dumps_func, combined_kwargs, nullptr);
    Py_DECREF(dumps_func);
    Py_DECREF(json_module);
    Py_DECREF(combined_kwargs);

    if (!json_str) {
        PyErr_Clear();
        return "{}";
    }

    const char* json_cstr = PyUnicode_AsUTF8(json_str);
    std::string result = json_cstr ? json_cstr : "{}";

    Py_DECREF(json_str);

    return result;
}

bool ProcessPoolExecutor::serialize_kwargs_binary(
    PyObject* args,
    PyObject* kwargs,
    PooledBuffer& buffer,
    size_t& out_size
) {
    // Build kwargs dict from args and kwargs
    PyObject* combined_kwargs = PyDict_New();
    if (!combined_kwargs) {
        return false;
    }

    // Merge kwargs if provided
    if (kwargs && PyDict_Check(kwargs)) {
        PyDict_Update(combined_kwargs, kwargs);
    }

    BinaryKwargsEncoder encoder(buffer);
    encoder.begin();

    // Iterate over dict items
    PyObject* key = nullptr;
    PyObject* value = nullptr;
    Py_ssize_t pos = 0;

    // Lazy-load json module only if we need fallback serialization
    PyObject* json_module = nullptr;
    PyObject* dumps_func = nullptr;

    while (PyDict_Next(combined_kwargs, &pos, &key, &value)) {
        // Get key as string
        if (!PyUnicode_Check(key)) {
            continue;  // Skip non-string keys
        }

        Py_ssize_t key_len;
        const char* key_str = PyUnicode_AsUTF8AndSize(key, &key_len);
        if (!key_str || key_len > 255) {
            continue;  // Skip invalid or too-long keys
        }
        std::string_view name(key_str, static_cast<size_t>(key_len));

        // Encode value based on type
        if (value == Py_None) {
            encoder.add_null(name);
        }
        else if (PyBool_Check(value)) {
            encoder.add_bool(name, value == Py_True);
        }
        else if (PyLong_Check(value)) {
            // Check if fits in int64
            int overflow = 0;
            int64_t int_val = PyLong_AsLongLongAndOverflow(value, &overflow);
            if (overflow == 0 && !PyErr_Occurred()) {
                encoder.add_int(name, int_val);
            } else {
                // Try unsigned
                PyErr_Clear();
                uint64_t uint_val = PyLong_AsUnsignedLongLong(value);
                if (!PyErr_Occurred()) {
                    encoder.add_uint(name, uint_val);
                } else {
                    // Fallback to JSON for very large integers
                    PyErr_Clear();
                    goto fallback_json;
                }
            }
        }
        else if (PyFloat_Check(value)) {
            encoder.add_float(name, PyFloat_AS_DOUBLE(value));
        }
        else if (PyUnicode_Check(value)) {
            Py_ssize_t str_len;
            const char* str_val = PyUnicode_AsUTF8AndSize(value, &str_len);
            if (str_val) {
                encoder.add_string(name, std::string_view(str_val, static_cast<size_t>(str_len)));
            }
        }
        else if (PyBytes_Check(value)) {
            char* bytes_ptr;
            Py_ssize_t bytes_len;
            if (PyBytes_AsStringAndSize(value, &bytes_ptr, &bytes_len) == 0) {
                encoder.add_bytes(name, reinterpret_cast<const uint8_t*>(bytes_ptr),
                                 static_cast<size_t>(bytes_len));
            }
        }
        else {
            // Complex type (list, dict, tuple, etc.) - fallback to JSON
        fallback_json:
            if (!json_module) {
                json_module = PyImport_ImportModule("json");
                if (!json_module) {
                    PyErr_Clear();
                    continue;
                }
                dumps_func = PyObject_GetAttrString(json_module, "dumps");
                if (!dumps_func) {
                    Py_DECREF(json_module);
                    json_module = nullptr;
                    PyErr_Clear();
                    continue;
                }
            }

            PyObject* json_str = PyObject_CallFunctionObjArgs(dumps_func, value, nullptr);
            if (json_str && PyUnicode_Check(json_str)) {
                Py_ssize_t json_len;
                const char* json_val = PyUnicode_AsUTF8AndSize(json_str, &json_len);
                if (json_val) {
                    encoder.add_json_fallback(name, std::string_view(json_val, static_cast<size_t>(json_len)));
                }
                Py_DECREF(json_str);
            } else {
                Py_XDECREF(json_str);
                PyErr_Clear();
            }
        }
    }

    // Cleanup
    Py_XDECREF(dumps_func);
    Py_XDECREF(json_module);
    Py_DECREF(combined_kwargs);

    out_size = encoder.finish();
    return out_size > 0;
}

PyObject* ProcessPoolExecutor::deserialize_response(const std::string& body_json) {
    // Parse JSON response
    PyObject* json_module = PyImport_ImportModule("json");
    if (!json_module) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }

    PyObject* loads_func = PyObject_GetAttrString(json_module, "loads");
    if (!loads_func) {
        Py_DECREF(json_module);
        PyErr_Clear();
        Py_RETURN_NONE;
    }

    PyObject* json_str = PyUnicode_FromString(body_json.c_str());
    if (!json_str) {
        Py_DECREF(loads_func);
        Py_DECREF(json_module);
        PyErr_Clear();
        Py_RETURN_NONE;
    }

    PyObject* result = PyObject_CallFunctionObjArgs(loads_func, json_str, nullptr);

    Py_DECREF(json_str);
    Py_DECREF(loads_func);
    Py_DECREF(json_module);

    if (!result) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }

    return result;
}

ProcessPoolExecutor& ProcessPoolExecutor::instance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (!instance_) {
        instance_ = std::unique_ptr<ProcessPoolExecutor>(new ProcessPoolExecutor());
    }
    return *instance_;
}

void ProcessPoolExecutor::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (instance_) {
        // Check if config differs - if so, reinitialize
        if (instance_->config_.num_workers != config.num_workers ||
            instance_->config_.use_zeromq != config.use_zeromq ||
            instance_->config_.python_executable != config.python_executable) {
            LOG_INFO("ProcessPoolExecutor", "Reinitializing with new config (workers: %u -> %u)",
                     instance_->config_.num_workers, config.num_workers);
            // Shutdown existing instance (don't hold lock during shutdown)
            auto old_instance = std::move(instance_);
            instance_ = nullptr;
            // Release lock temporarily for clean shutdown
            instance_mutex_.unlock();
            old_instance->shutdown();
            old_instance.reset();
            instance_mutex_.lock();
        } else {
            LOG_INFO("ProcessPoolExecutor", "Instance already initialized with same config, reusing");
            return;
        }
    }
    instance_ = std::unique_ptr<ProcessPoolExecutor>(new ProcessPoolExecutor(config));
}

void ProcessPoolExecutor::reset() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (instance_) {
        LOG_INFO("ProcessPoolExecutor", "Resetting ProcessPoolExecutor");
        // Move to local to avoid holding lock during shutdown
        auto old_instance = std::move(instance_);
        instance_ = nullptr;
        // Release lock temporarily for clean shutdown
        instance_mutex_.unlock();
        old_instance->shutdown();
        old_instance.reset();
        instance_mutex_.lock();
        LOG_INFO("ProcessPoolExecutor", "ProcessPoolExecutor reset complete");
    }
}

ProcessPoolExecutor::StatsSnapshot ProcessPoolExecutor::get_stats() const {
    StatsSnapshot result;
    result.tasks_submitted = stats_.tasks_submitted.load(std::memory_order_relaxed);
    result.tasks_completed = stats_.tasks_completed.load(std::memory_order_relaxed);
    result.tasks_failed = stats_.tasks_failed.load(std::memory_order_relaxed);
    result.tasks_timeout = stats_.tasks_timeout.load(std::memory_order_relaxed);
    return result;
}

ProcessPoolExecutor* ProcessPoolExecutor::get_instance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    return instance_.get();
}

bool ProcessPoolExecutor::send_ws_event(MessageType type,
                                        uint64_t connection_id,
                                        const std::string& path,
                                        const std::string& payload,
                                        bool is_binary) {
#ifdef FASTERAPI_USE_ZMQ
    if (zmq_ipc_) {
        return zmq_ipc_->write_ws_event(type, connection_id, path, payload, is_binary);
    }
#endif
    LOG_WARN("ProcessPoolExecutor", "send_ws_event called but ZMQ IPC not available");
    return false;
}

}  // namespace python
}  // namespace fasterapi
