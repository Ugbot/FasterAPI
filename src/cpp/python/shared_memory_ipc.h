#pragma once

#include "ipc_protocol.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>
#include <semaphore.h>
#include <sys/mman.h>

namespace fasterapi {
namespace python {

// Ring buffer slot
struct RingBufferSlot {
    std::atomic<uint32_t> length;  // 0 = empty, >0 = message length
    uint8_t data[4096];  // Max message size per slot
};

// Control block for the ring buffer
struct RingBufferControl {
    std::atomic<uint32_t> head;  // Write position
    std::atomic<uint32_t> tail;  // Read position
    uint32_t capacity;  // Number of slots
    sem_t* write_sem;  // Semaphore for space available
    sem_t* read_sem;   // Semaphore for data available
};

// Shared memory region layout
struct SharedMemoryRegion {
    // Control blocks
    RingBufferControl request_control;
    RingBufferControl response_control;

    // Ring buffers (variable size, allocated after this struct)
    // RingBufferSlot request_slots[REQUEST_QUEUE_SIZE];
    // RingBufferSlot response_slots[RESPONSE_QUEUE_SIZE];
};

/**
 * Manages shared memory for IPC between C++ server and Python worker processes.
 * Uses ring buffers with semaphores for lock-free communication.
 */
class SharedMemoryIPC {
public:
    static constexpr size_t DEFAULT_QUEUE_SIZE = 256;
    static constexpr size_t DEFAULT_SHM_SIZE = 16 * 1024 * 1024;  // 16MB

    /**
     * Create shared memory region (master/server side).
     * @param name Unique name for the shared memory object
     * @param request_queue_size Number of slots in request queue
     * @param response_queue_size Number of slots in response queue
     */
    SharedMemoryIPC(const std::string& name,
                    size_t request_queue_size = DEFAULT_QUEUE_SIZE,
                    size_t response_queue_size = DEFAULT_QUEUE_SIZE);

    /**
     * Attach to existing shared memory region (worker side).
     * @param name Name of the shared memory object to attach to
     */
    static std::unique_ptr<SharedMemoryIPC> attach(const std::string& name);

    ~SharedMemoryIPC();

    // Disable copy
    SharedMemoryIPC(const SharedMemoryIPC&) = delete;
    SharedMemoryIPC& operator=(const SharedMemoryIPC&) = delete;

    /**
     * Write a request to the shared memory queue.
     * Blocks if queue is full.
     * @return true if successful, false on error
     */
    bool write_request(uint32_t request_id,
                      const std::string& module_name,
                      const std::string& function_name,
                      const std::string& kwargs_json);

    /**
     * Read a request from the shared memory queue.
     * Blocks if queue is empty.
     * @return true if successful, false on error or shutdown signal
     */
    bool read_request(uint32_t& request_id,
                     std::string& module_name,
                     std::string& function_name,
                     std::string& kwargs_json);

    /**
     * Write a response to the shared memory queue.
     * Blocks if queue is full.
     */
    bool write_response(uint32_t request_id,
                       uint16_t status_code,
                       bool success,
                       const std::string& body_json,
                       const std::string& error_message = "");

    /**
     * Read a response from the shared memory queue.
     * Blocks if queue is empty.
     */
    bool read_response(uint32_t& request_id,
                      uint16_t& status_code,
                      bool& success,
                      std::string& body_json,
                      std::string& error_message);

    /**
     * Signal shutdown to all workers.
     */
    void signal_shutdown();

    /**
     * Wake the response reader thread (used during shutdown).
     * Posts to response semaphore to unblock any waiting read_response() call.
     */
    void wake_response_reader();

    /**
     * Get the shared memory name (for passing to workers).
     */
    const std::string& get_name() const { return shm_name_; }

    /**
     * Check if this is the master (creator) or worker (attached).
     */
    bool is_master() const { return is_master_; }

private:
    std::string shm_name_;
    int shm_fd_;
    void* shm_ptr_;
    size_t shm_size_;
    bool is_master_;

    SharedMemoryRegion* region_;
    RingBufferSlot* request_slots_;
    RingBufferSlot* response_slots_;
    size_t request_queue_size_;
    size_t response_queue_size_;

    // Semaphores (named for cross-process access)
    sem_t* req_write_sem_;
    sem_t* req_read_sem_;
    sem_t* resp_write_sem_;
    sem_t* resp_read_sem_;

    std::atomic<bool> shutdown_;

    // Private constructor for attach()
    SharedMemoryIPC(const std::string& name, bool is_master);

    // Initialize the shared memory region
    bool initialize();

    // Attach to existing region
    bool attach_existing();

    // Helper to write to ring buffer
    bool write_to_ring(RingBufferControl& control,
                      RingBufferSlot* slots,
                      const uint8_t* data,
                      uint32_t length);

    // Helper to read from ring buffer
    bool read_from_ring(RingBufferControl& control,
                       RingBufferSlot* slots,
                       uint8_t* buffer,
                       uint32_t& length,
                       uint32_t max_length);
};

}  // namespace python
}  // namespace fasterapi
