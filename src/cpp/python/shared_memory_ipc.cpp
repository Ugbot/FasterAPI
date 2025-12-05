#include "shared_memory_ipc.h"
#include <cstring>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

namespace fasterapi {
namespace python {

SharedMemoryIPC::SharedMemoryIPC(const std::string& name,
                                 size_t request_queue_size,
                                 size_t response_queue_size)
    : shm_name_(name)
    , shm_fd_(-1)
    , shm_ptr_(nullptr)
    , is_master_(true)
    , region_(nullptr)
    , request_slots_(nullptr)
    , response_slots_(nullptr)
    , request_queue_size_(request_queue_size)
    , response_queue_size_(response_queue_size)
    , req_write_sem_(nullptr)
    , req_read_sem_(nullptr)
    , resp_write_sem_(nullptr)
    , resp_read_sem_(nullptr)
    , shutdown_(false) {

    // Calculate required size
    shm_size_ = sizeof(SharedMemoryRegion) +
                (request_queue_size * sizeof(RingBufferSlot)) +
                (response_queue_size * sizeof(RingBufferSlot));

    if (!initialize()) {
        std::cerr << "FATAL: Failed to initialize shared memory: " << strerror(errno) << std::endl;
        std::abort();
    }
}

SharedMemoryIPC::SharedMemoryIPC(const std::string& name, bool is_master)
    : shm_name_(name)
    , shm_fd_(-1)
    , shm_ptr_(nullptr)
    , is_master_(is_master)
    , region_(nullptr)
    , request_slots_(nullptr)
    , response_slots_(nullptr)
    , req_write_sem_(nullptr)
    , req_read_sem_(nullptr)
    , resp_write_sem_(nullptr)
    , resp_read_sem_(nullptr)
    , shutdown_(false) {
}

std::unique_ptr<SharedMemoryIPC> SharedMemoryIPC::attach(const std::string& name) {
    auto ipc = std::unique_ptr<SharedMemoryIPC>(new SharedMemoryIPC(name, false));
    if (!ipc->attach_existing()) {
        return nullptr;
    }
    return ipc;
}

SharedMemoryIPC::~SharedMemoryIPC() {
    if (shm_ptr_ != nullptr) {
        munmap(shm_ptr_, shm_size_);
    }

    if (is_master_) {
        // Master cleans up resources
        if (shm_fd_ >= 0) {
            close(shm_fd_);
            shm_unlink(shm_name_.c_str());
        }

        // Clean up semaphores
        if (req_write_sem_) {
            sem_close(req_write_sem_);
            sem_unlink((shm_name_ + "_req_write").c_str());
        }
        if (req_read_sem_) {
            sem_close(req_read_sem_);
            sem_unlink((shm_name_ + "_req_read").c_str());
        }
        if (resp_write_sem_) {
            sem_close(resp_write_sem_);
            sem_unlink((shm_name_ + "_resp_write").c_str());
        }
        if (resp_read_sem_) {
            sem_close(resp_read_sem_);
            sem_unlink((shm_name_ + "_resp_read").c_str());
        }
    } else {
        // Worker just closes handles
        if (shm_fd_ >= 0) {
            close(shm_fd_);
        }
        if (req_write_sem_) sem_close(req_write_sem_);
        if (req_read_sem_) sem_close(req_read_sem_);
        if (resp_write_sem_) sem_close(resp_write_sem_);
        if (resp_read_sem_) sem_close(resp_read_sem_);
    }
}

bool SharedMemoryIPC::initialize() {
    // Create shared memory object
    shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shm_fd_ == -1) {
        std::cerr << "shm_open failed: " << strerror(errno) << std::endl;
        return false;
    }

    // Set size
    if (ftruncate(shm_fd_, shm_size_) == -1) {
        std::cerr << "ftruncate failed: " << strerror(errno) << std::endl;
        close(shm_fd_);
        shm_unlink(shm_name_.c_str());
        return false;
    }

    // Map memory
    shm_ptr_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << std::endl;
        close(shm_fd_);
        shm_unlink(shm_name_.c_str());
        return false;
    }

    // Zero out memory
    memset(shm_ptr_, 0, shm_size_);

    // Set up pointers
    region_ = static_cast<SharedMemoryRegion*>(shm_ptr_);
    request_slots_ = reinterpret_cast<RingBufferSlot*>(
        static_cast<uint8_t*>(shm_ptr_) + sizeof(SharedMemoryRegion));
    response_slots_ = request_slots_ + request_queue_size_;

    // Initialize control blocks
    region_->request_control.head.store(0, std::memory_order_release);
    region_->request_control.tail.store(0, std::memory_order_release);
    region_->request_control.capacity = request_queue_size_;

    region_->response_control.head.store(0, std::memory_order_release);
    region_->response_control.tail.store(0, std::memory_order_release);
    region_->response_control.capacity = response_queue_size_;

    // Initialize ring buffer slots
    for (size_t i = 0; i < request_queue_size_; ++i) {
        request_slots_[i].length.store(0, std::memory_order_release);
    }
    for (size_t i = 0; i < response_queue_size_; ++i) {
        response_slots_[i].length.store(0, std::memory_order_release);
    }

    // Create semaphores
    req_write_sem_ = sem_open((shm_name_ + "_req_write").c_str(), O_CREAT | O_EXCL, 0600, request_queue_size_);
    req_read_sem_ = sem_open((shm_name_ + "_req_read").c_str(), O_CREAT | O_EXCL, 0600, 0);
    resp_write_sem_ = sem_open((shm_name_ + "_resp_write").c_str(), O_CREAT | O_EXCL, 0600, response_queue_size_);
    resp_read_sem_ = sem_open((shm_name_ + "_resp_read").c_str(), O_CREAT | O_EXCL, 0600, 0);

    if (req_write_sem_ == SEM_FAILED || req_read_sem_ == SEM_FAILED ||
        resp_write_sem_ == SEM_FAILED || resp_read_sem_ == SEM_FAILED) {
        std::cerr << "sem_open failed: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

bool SharedMemoryIPC::attach_existing() {
    // Open existing shared memory
    shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0600);
    if (shm_fd_ == -1) {
        std::cerr << "shm_open (attach) failed: " << strerror(errno) << std::endl;
        return false;
    }

    // Get size
    struct stat shm_stat;
    if (fstat(shm_fd_, &shm_stat) == -1) {
        std::cerr << "fstat failed: " << strerror(errno) << std::endl;
        close(shm_fd_);
        return false;
    }
    shm_size_ = shm_stat.st_size;

    // Map memory
    shm_ptr_ = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (shm_ptr_ == MAP_FAILED) {
        std::cerr << "mmap (attach) failed: " << strerror(errno) << std::endl;
        close(shm_fd_);
        return false;
    }

    // Set up pointers
    region_ = static_cast<SharedMemoryRegion*>(shm_ptr_);
    request_queue_size_ = region_->request_control.capacity;
    response_queue_size_ = region_->response_control.capacity;

    request_slots_ = reinterpret_cast<RingBufferSlot*>(
        static_cast<uint8_t*>(shm_ptr_) + sizeof(SharedMemoryRegion));
    response_slots_ = request_slots_ + request_queue_size_;

    // Open semaphores
    req_write_sem_ = sem_open((shm_name_ + "_req_write").c_str(), 0);
    req_read_sem_ = sem_open((shm_name_ + "_req_read").c_str(), 0);
    resp_write_sem_ = sem_open((shm_name_ + "_resp_write").c_str(), 0);
    resp_read_sem_ = sem_open((shm_name_ + "_resp_read").c_str(), 0);

    if (req_write_sem_ == SEM_FAILED || req_read_sem_ == SEM_FAILED ||
        resp_write_sem_ == SEM_FAILED || resp_read_sem_ == SEM_FAILED) {
        std::cerr << "sem_open (attach) failed: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

bool SharedMemoryIPC::write_request(uint32_t request_id,
                                   const std::string& module_name,
                                   const std::string& function_name,
                                   const std::string& kwargs_json) {
    // Build message
    MessageHeader header;
    header.type = MessageType::REQUEST;
    header.request_id = request_id;
    header.module_name_len = module_name.length();
    header.function_name_len = function_name.length();
    header.kwargs_json_len = kwargs_json.length();
    header.total_length = sizeof(MessageHeader) +
                         module_name.length() +
                         function_name.length() +
                         kwargs_json.length();

    // Check size
    if (header.total_length > sizeof(RingBufferSlot::data)) {
        std::cerr << "Message too large: " << header.total_length << " bytes" << std::endl;
        return false;
    }

    // Pack message
    uint8_t buffer[sizeof(RingBufferSlot::data)];
    uint8_t* ptr = buffer;
    memcpy(ptr, &header, sizeof(header));
    ptr += sizeof(header);
    memcpy(ptr, module_name.data(), module_name.length());
    ptr += module_name.length();
    memcpy(ptr, function_name.data(), function_name.length());
    ptr += function_name.length();
    memcpy(ptr, kwargs_json.data(), kwargs_json.length());

    return write_to_ring(region_->request_control, request_slots_, buffer, header.total_length);
}

bool SharedMemoryIPC::read_request(uint32_t& request_id,
                                  std::string& module_name,
                                  std::string& function_name,
                                  std::string& kwargs_json) {
    uint8_t buffer[sizeof(RingBufferSlot::data)];
    uint32_t length = 0;

    if (!read_from_ring(region_->request_control, request_slots_, buffer, length, sizeof(buffer))) {
        return false;
    }

    // Parse message
    if (length < sizeof(MessageHeader)) {
        return false;
    }

    MessageHeader header;
    memcpy(&header, buffer, sizeof(header));

    if (header.type == MessageType::SHUTDOWN) {
        shutdown_.store(true, std::memory_order_release);
        return false;
    }

    if (header.type != MessageType::REQUEST) {
        return false;
    }

    request_id = header.request_id;

    const uint8_t* ptr = buffer + sizeof(MessageHeader);
    module_name.assign(reinterpret_cast<const char*>(ptr), header.module_name_len);
    ptr += header.module_name_len;
    function_name.assign(reinterpret_cast<const char*>(ptr), header.function_name_len);
    ptr += header.function_name_len;
    kwargs_json.assign(reinterpret_cast<const char*>(ptr), header.kwargs_json_len);

    return true;
}

bool SharedMemoryIPC::write_response(uint32_t request_id,
                                    uint16_t status_code,
                                    bool success,
                                    const std::string& body_json,
                                    const std::string& error_message) {
    // Build message
    ResponseHeader header;
    header.type = MessageType::RESPONSE;
    header.request_id = request_id;
    header.status_code = status_code;
    header.success = success ? 1 : 0;
    header.body_json_len = body_json.length();
    header.error_message_len = error_message.length();
    header.total_length = sizeof(ResponseHeader) +
                         body_json.length() +
                         error_message.length();

    // Check size
    if (header.total_length > sizeof(RingBufferSlot::data)) {
        std::cerr << "Response too large: " << header.total_length << " bytes" << std::endl;
        return false;
    }

    // Pack message
    uint8_t buffer[sizeof(RingBufferSlot::data)];
    uint8_t* ptr = buffer;
    memcpy(ptr, &header, sizeof(header));
    ptr += sizeof(header);
    memcpy(ptr, body_json.data(), body_json.length());
    ptr += body_json.length();
    memcpy(ptr, error_message.data(), error_message.length());

    return write_to_ring(region_->response_control, response_slots_, buffer, header.total_length);
}

bool SharedMemoryIPC::read_response(uint32_t& request_id,
                                   uint16_t& status_code,
                                   bool& success,
                                   std::string& body_json,
                                   std::string& error_message) {
    uint8_t buffer[sizeof(RingBufferSlot::data)];
    uint32_t length = 0;

    if (!read_from_ring(region_->response_control, response_slots_, buffer, length, sizeof(buffer))) {
        return false;
    }

    // Parse message
    if (length < sizeof(ResponseHeader)) {
        return false;
    }

    ResponseHeader header;
    memcpy(&header, buffer, sizeof(header));

    if (header.type != MessageType::RESPONSE) {
        return false;
    }

    request_id = header.request_id;
    status_code = header.status_code;
    success = (header.success != 0);

    const uint8_t* ptr = buffer + sizeof(ResponseHeader);
    body_json.assign(reinterpret_cast<const char*>(ptr), header.body_json_len);
    ptr += header.body_json_len;
    error_message.assign(reinterpret_cast<const char*>(ptr), header.error_message_len);

    return true;
}

void SharedMemoryIPC::signal_shutdown() {
    shutdown_.store(true, std::memory_order_release);

    // Send shutdown messages to all workers
    MessageHeader header;
    header.type = MessageType::SHUTDOWN;
    header.request_id = 0;
    header.total_length = sizeof(MessageHeader);
    header.module_name_len = 0;
    header.function_name_len = 0;
    header.kwargs_json_len = 0;

    // Send one shutdown message per worker (queue capacity)
    for (size_t i = 0; i < request_queue_size_; ++i) {
        write_to_ring(region_->request_control, request_slots_,
                     reinterpret_cast<uint8_t*>(&header), sizeof(header));
    }
}

void SharedMemoryIPC::wake_response_reader() {
    // Post to response read semaphore to wake any blocked read_response() call
    // This ensures the response reader thread can exit cleanly during shutdown
    if (resp_read_sem_) {
        sem_post(resp_read_sem_);
    }
}

bool SharedMemoryIPC::write_to_ring(RingBufferControl& control,
                                   RingBufferSlot* slots,
                                   const uint8_t* data,
                                   uint32_t length) {
    // Get semaphore (wait for space)
    sem_t* write_sem = (&control == &region_->request_control) ? req_write_sem_ : resp_write_sem_;
    sem_t* read_sem = (&control == &region_->request_control) ? req_read_sem_ : resp_read_sem_;

    if (sem_wait(write_sem) != 0) {
        return false;
    }

    // Get write position
    uint32_t head = control.head.load(std::memory_order_acquire);
    uint32_t next_head = (head + 1) % control.capacity;

    // Write data
    RingBufferSlot& slot = slots[head];
    memcpy(slot.data, data, length);
    slot.length.store(length, std::memory_order_release);

    // Advance head
    control.head.store(next_head, std::memory_order_release);

    // Signal reader
    sem_post(read_sem);

    return true;
}

bool SharedMemoryIPC::read_from_ring(RingBufferControl& control,
                                    RingBufferSlot* slots,
                                    uint8_t* buffer,
                                    uint32_t& length,
                                    uint32_t max_length) {
    // Get semaphore (wait for data)
    sem_t* write_sem = (&control == &region_->request_control) ? req_write_sem_ : resp_write_sem_;
    sem_t* read_sem = (&control == &region_->request_control) ? req_read_sem_ : resp_read_sem_;

    if (sem_wait(read_sem) != 0) {
        return false;
    }

    // Check for shutdown
    if (shutdown_.load(std::memory_order_acquire)) {
        return false;
    }

    // Get read position
    uint32_t tail = control.tail.load(std::memory_order_acquire);
    uint32_t next_tail = (tail + 1) % control.capacity;

    // Read data
    RingBufferSlot& slot = slots[tail];
    length = slot.length.load(std::memory_order_acquire);

    if (length > max_length) {
        std::cerr << "Buffer too small for message" << std::endl;
        return false;
    }

    memcpy(buffer, slot.data, length);

    // Clear slot
    slot.length.store(0, std::memory_order_release);

    // Advance tail
    control.tail.store(next_tail, std::memory_order_release);

    // Signal writer
    sem_post(write_sem);

    return true;
}

}  // namespace python
}  // namespace fasterapi
