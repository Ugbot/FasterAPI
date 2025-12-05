#pragma once

#include "shared_memory_ipc.h"
#include "binary_kwargs.h"
#ifdef FASTERAPI_USE_ZMQ
#include "zmq_ipc.h"
#endif
#include "../core/future.h"
#include "../core/result.h"
#include <Python.h>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <functional>
#include "../core/lockfree_queue.h"

namespace fasterapi {
namespace python {

using namespace fasterapi::core;

/**
 * Process pool executor for true multi-core Python execution.
 *
 * Spawns N worker processes, each running an asyncio event loop.
 * Communicates via shared memory for minimal overhead (~1-3µs per request).
 *
 * Benefits over sub-interpreters:
 * - No GIL at all (processes are independent)
 * - No PyObject* sharing issues (uses JSON serialization)
 * - Better crash isolation (handler crash doesn't take down server)
 * - Simpler architecture (standard multiprocessing model)
 *
 * Architecture:
 * ┌─────────────────┐
 * │  C++ Server     │ ← HTTP/routing/parameter extraction
 * │  (this process) │
 * └────────┬────────┘
 *          │ Shared Memory IPC
 *    ┌─────┴─────┬─────────┬─────────┐
 *    ▼           ▼         ▼         ▼
 * ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐
 * │ Py 1 │  │ Py 2 │  │ Py 3 │  │ Py N │  ← Python workers (asyncio)
 * └──────┘  └──────┘  └──────┘  └──────┘
 */
class ProcessPoolExecutor {
public:
    /**
     * Executor configuration.
     */
    struct Config {
        uint32_t num_workers;           // Number of worker processes (0 = auto detect CPUs)
        std::string python_executable;  // Path to python executable (default: python3.13)
        std::string project_dir;        // Project directory to add to sys.path
        bool pin_to_cores;              // CPU affinity (platform-specific)
        bool use_zeromq;                // Use ZeroMQ IPC (default: true, shared memory deprecated)

        Config()
            : num_workers(0),  // 0 = std::thread::hardware_concurrency()
              python_executable("python3.13"),
              project_dir("."),
              pin_to_cores(false),
              use_zeromq(true) {}
    };

    /**
     * Statistics (internal, uses atomics).
     */
    struct Stats {
        std::atomic<uint64_t> tasks_submitted{0};
        std::atomic<uint64_t> tasks_completed{0};
        std::atomic<uint64_t> tasks_failed{0};
        std::atomic<uint64_t> tasks_timeout{0};
    };

    /**
     * Statistics snapshot (copyable, for returning to caller).
     */
    struct StatsSnapshot {
        uint64_t tasks_submitted{0};
        uint64_t tasks_completed{0};
        uint64_t tasks_failed{0};
        uint64_t tasks_timeout{0};
    };

    /**
     * Create process pool executor.
     *
     * @param config Configuration
     * @throws std::runtime_error if initialization fails
     */
    explicit ProcessPoolExecutor(const Config& config = Config());

    /**
     * Destructor. Shuts down workers and cleans up resources.
     */
    ~ProcessPoolExecutor();

    // Disable copy/move
    ProcessPoolExecutor(const ProcessPoolExecutor&) = delete;
    ProcessPoolExecutor& operator=(const ProcessPoolExecutor&) = delete;

    /**
     * Submit a task for execution using handler metadata.
     *
     * This is the primary API for handler execution, compatible with SubinterpreterExecutor.
     * Serializes kwargs to JSON before sending to worker process.
     *
     * @param module_name Python module containing the handler (e.g., "myapp.handlers")
     * @param function_name Handler function name (e.g., "get_user")
     * @param args Positional arguments (will be converted to kwargs if possible, or serialized)
     * @param kwargs Keyword arguments dict
     * @return future<result<PyObject*>> Future that resolves when handler completes
     *
     * Note: kwargs will be JSON-serialized for IPC, then deserialized in worker.
     * Only JSON-compatible types are supported (int, float, str, bool, list, dict, None).
     */
    static future<result<PyObject*>> submit_with_metadata(
        const std::string& module_name,
        const std::string& function_name,
        PyObject* args = nullptr,
        PyObject* kwargs = nullptr
    );

    /**
     * Get singleton instance (initialized on first use).
     */
    static ProcessPoolExecutor& instance();

    /**
     * Get singleton instance pointer (returns nullptr if not initialized).
     */
    static ProcessPoolExecutor* get_instance();

    /**
     * Initialize singleton with custom config.
     * If already initialized, shuts down existing instance and reinitializes.
     * Must be called before first use of instance().
     */
    static void initialize(const Config& config);

    /**
     * Reset (shutdown and destroy) the singleton instance.
     * After calling this, initialize() can be called with new config.
     */
    static void reset();

    /**
     * Send WebSocket event to Python workers.
     *
     * @param type Event type (WS_CONNECT, WS_MESSAGE, WS_DISCONNECT)
     * @param connection_id WebSocket connection ID
     * @param path WebSocket path
     * @param payload Message payload
     * @param is_binary True for binary payload
     * @return True if sent successfully
     */
    bool send_ws_event(MessageType type,
                       uint64_t connection_id,
                       const std::string& path,
                       const std::string& payload = "",
                       bool is_binary = false);

    /**
     * WebSocket response from Python worker (for lock-free queue).
     * Designed for zero-copy where possible.
     */
    struct WsResponse {
        MessageType type;           // WS_SEND or WS_CLOSE
        uint64_t connection_id;     // Target WebSocket connection
        std::string payload;        // Message payload (moved, not copied)
        bool is_binary;             // Binary or text frame
        uint16_t close_code;        // Close code (for WS_CLOSE)

        WsResponse() : type(MessageType::WS_SEND), connection_id(0), is_binary(false), close_code(0) {}

        WsResponse(MessageType t, uint64_t conn_id, std::string&& p, bool binary, uint16_t code)
            : type(t), connection_id(conn_id), payload(std::move(p)), is_binary(binary), close_code(code) {}

        // Move semantics for lock-free queue
        WsResponse(WsResponse&& other) noexcept = default;
        WsResponse& operator=(WsResponse&& other) noexcept = default;
        WsResponse(const WsResponse&) = default;
        WsResponse& operator=(const WsResponse&) = default;
    };

    /**
     * Poll WebSocket responses from Python workers (lock-free).
     * Call this from the event loop to dispatch WebSocket frames.
     *
     * @param response Output: the next WebSocket response
     * @return true if a response was available, false if queue is empty
     */
    bool poll_ws_response(WsResponse& response);

    /**
     * Check if there are pending WebSocket responses.
     * Non-blocking, approximate (may return false positives).
     */
    bool has_ws_responses() const;

    /**
     * Shutdown executor (called automatically on destruction).
     */
    void shutdown();

    /**
     * Get statistics.
     */
    StatsSnapshot get_stats() const;

private:
    Config config_;
    Stats stats_;

    // IPC (dual-mode: shared memory or ZeroMQ)
    std::unique_ptr<SharedMemoryIPC> shm_ipc_;
#ifdef FASTERAPI_USE_ZMQ
    std::unique_ptr<ZmqIPC> zmq_ipc_;
#endif
    std::string ipc_id_;  // Shared memory name or ZMQ IPC prefix

    // Worker processes
    std::vector<pid_t> worker_pids_;
    std::atomic<bool> shutdown_;

    // Request ID generation
    std::atomic<uint32_t> next_request_id_{1};

    // Pending requests: request_id -> promise
    std::unordered_map<uint32_t, promise<result<PyObject*>>> pending_requests_;
    std::mutex pending_mutex_;

    // Response reader thread
    std::thread response_thread_;

    // WebSocket response queue (lock-free, SPSC: response thread -> event loop)
    // Using SPSC since only response_reader_loop pushes and event loop pops
    std::unique_ptr<AeronSPSCQueue<WsResponse>> ws_response_queue_;

    // Singleton
    static std::unique_ptr<ProcessPoolExecutor> instance_;
    static std::mutex instance_mutex_;

    // Helper methods
    void start_workers();
    void stop_workers();
    void response_reader_loop();
    uint32_t generate_request_id();
    std::string serialize_kwargs(PyObject* args, PyObject* kwargs);
    bool serialize_kwargs_binary(PyObject* args, PyObject* kwargs,
                                 PooledBuffer& buffer, size_t& out_size);
    PyObject* deserialize_response(const std::string& body_json);

    // Handle HTTP response from worker
    void handle_http_response(uint32_t request_id, uint16_t status_code, bool success,
                              const std::string& body_json, const std::string& error_message);

    // Internal implementation (non-static version)
    future<result<PyObject*>> submit_with_metadata_impl(
        const std::string& module_name,
        const std::string& function_name,
        PyObject* args,
        PyObject* kwargs
    );
};

}  // namespace python
}  // namespace fasterapi
