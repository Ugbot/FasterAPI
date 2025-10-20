#include "reactor.h"
#include "task.h"
#include <iostream>
#include <chrono>
#include <queue>
#include <map>
#include <algorithm>

#ifdef __linux__
#include <sys/epoll.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <unistd.h>
#elif defined(_WIN32)
// Windows IOCP support would go here
#endif

namespace fasterapi {
namespace core {

// Implement reactor_deleter
void reactor_deleter::operator()(reactor* r) const noexcept {
    delete r;
}

// Global reactor registry
static std::vector<std::unique_ptr<reactor, reactor_deleter>> g_reactors;
static std::atomic<uint32_t> g_num_cores{0};
static thread_local reactor* g_local_reactor = nullptr;
static thread_local uint32_t g_local_core_id = 0;

// Task queue implementation
struct reactor::task_queue {
    std::mutex mutex;
    std::vector<task*> tasks;
    std::atomic<uint64_t> pending{0};
    
    void push(task* t) {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.push_back(t);
        pending.fetch_add(1, std::memory_order_relaxed);
    }
    
    std::vector<task*> pop_all() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<task*> result;
        result.swap(tasks);
        pending.store(0, std::memory_order_relaxed);
        return result;
    }
};

// Timer queue implementation
struct reactor::timer_queue {
    std::mutex mutex;
    std::map<uint64_t, std::vector<task*>> timers;  // when_ns -> tasks
    std::atomic<uint64_t> next_id{1};
    std::map<uint64_t, uint64_t> id_to_time;  // timer_id -> when_ns
    
    uint64_t add(uint64_t when_ns, task* t) {
        std::lock_guard<std::mutex> lock(mutex);
        uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
        timers[when_ns].push_back(t);
        id_to_time[id] = when_ns;
        return id;
    }
    
    int cancel(uint64_t timer_id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = id_to_time.find(timer_id);
        if (it == id_to_time.end()) {
            return 1;  // Timer not found
        }
        id_to_time.erase(it);
        return 0;
    }
    
    std::vector<task*> pop_ready(uint64_t now_ns) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<task*> ready;
        
        auto it = timers.begin();
        while (it != timers.end() && it->first <= now_ns) {
            for (auto* t : it->second) {
                ready.push_back(t);
            }
            it = timers.erase(it);
        }
        
        return ready;
    }
};

reactor::reactor(uint32_t core_id) 
    : core_id_(core_id),
      task_queue_(std::make_unique<task_queue>()),
      timer_queue_(std::make_unique<timer_queue>()) {
    
#ifdef __linux__
    // Create epoll instance
    event_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (event_fd_ < 0) {
        std::cerr << "Failed to create epoll instance for core " << core_id_ << std::endl;
    }
#elif defined(__APPLE__)
    // Create kqueue instance
    event_fd_ = kqueue();
    if (event_fd_ < 0) {
        std::cerr << "Failed to create kqueue instance for core " << core_id_ << std::endl;
    }
#endif
}

reactor::~reactor() {
    stop();
    
    if (event_fd_ >= 0) {
        close(event_fd_);
    }
    
    // Clean up pending tasks
    auto tasks = task_queue_->pop_all();
    for (auto* t : tasks) {
        delete t;
    }
}

reactor& reactor::local() {
    if (!g_local_reactor) {
        std::cerr << "No reactor initialized for this thread" << std::endl;
        std::abort();
    }
    return *g_local_reactor;
}

int reactor::initialize(uint32_t num_cores) noexcept {
    if (g_num_cores.load() > 0) {
        return 1;  // Already initialized
    }
    
    if (num_cores == 0) {
        num_cores = std::thread::hardware_concurrency();
        if (num_cores == 0) num_cores = 1;
    }
    
    g_reactors.reserve(num_cores);
    for (uint32_t i = 0; i < num_cores; ++i) {
        g_reactors.emplace_back(new reactor(i), reactor_deleter{});
    }
    
    g_num_cores.store(num_cores, std::memory_order_release);
    
    // Set local reactor for main thread
    g_local_reactor = g_reactors[0].get();
    g_local_core_id = 0;
    
    return 0;
}

int reactor::shutdown() noexcept {
    for (auto& r : g_reactors) {
        if (r) {
            r->stop();
        }
    }
    
    g_reactors.clear();
    g_num_cores.store(0, std::memory_order_release);
    g_local_reactor = nullptr;
    
    return 0;
}

reactor* reactor::get(uint32_t core_id) noexcept {
    if (core_id >= g_reactors.size()) {
        return nullptr;
    }
    return g_reactors[core_id].get();
}

uint32_t reactor::current_core() noexcept {
    return g_local_core_id;
}

uint32_t reactor::num_cores() noexcept {
    return g_num_cores.load(std::memory_order_acquire);
}

void reactor::schedule(task* t) noexcept {
    if (!t) return;
    task_queue_->push(t);
}

void reactor::run() noexcept {
    if (running_.exchange(true)) {
        return;  // Already running
    }
    
    // Set this reactor as local for this thread
    g_local_reactor = this;
    g_local_core_id = core_id_;
    
    stop_requested_.store(false);
    
    std::cout << "Reactor " << core_id_ << " starting on thread " 
              << std::this_thread::get_id() << std::endl;
    
    while (!stop_requested_.load(std::memory_order_acquire)) {
        loops_.fetch_add(1, std::memory_order_relaxed);
        
        // Process tasks
        process_tasks();
        
        // Process timers
        process_timers();
        
        // Poll I/O events (with timeout)
        process_io_events(1);  // 1ms timeout
        
        // Yield if no work
        if (task_queue_->pending.load(std::memory_order_relaxed) == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    running_.store(false);
    std::cout << "Reactor " << core_id_ << " stopped" << std::endl;
}

void reactor::stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
}

bool reactor::is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

int reactor::event_fd() const noexcept {
    return event_fd_;
}

uint64_t reactor::add_timer(uint64_t when_ns, task* t) noexcept {
    if (!t) return 0;
    return timer_queue_->add(when_ns, t);
}

int reactor::cancel_timer(uint64_t timer_id) noexcept {
    return timer_queue_->cancel(timer_id);
}

uint64_t reactor::now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

reactor::stats reactor::get_stats() const noexcept {
    stats s;
    s.tasks_executed = tasks_executed_.load(std::memory_order_relaxed);
    s.tasks_pending = task_queue_->pending.load(std::memory_order_relaxed);
    s.io_events = io_events_.load(std::memory_order_relaxed);
    s.timers_fired = timers_fired_.load(std::memory_order_relaxed);
    s.loops = loops_.load(std::memory_order_relaxed);
    return s;
}

void reactor::process_tasks() noexcept {
    auto tasks = task_queue_->pop_all();
    
    for (auto* t : tasks) {
        t->run();
        tasks_executed_.fetch_add(1, std::memory_order_relaxed);
        delete t;
    }
}

void reactor::process_io_events(int timeout_ms) noexcept {
    if (event_fd_ < 0) return;
    
#ifdef __linux__
    struct epoll_event events[32];
    int n = epoll_wait(event_fd_, events, 32, timeout_ms);
    if (n > 0) {
        io_events_.fetch_add(n, std::memory_order_relaxed);
    }
#elif defined(__APPLE__)
    struct kevent events[32];
    struct timespec timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = (timeout_ms % 1000) * 1000000;
    
    int n = kevent(event_fd_, nullptr, 0, events, 32, &timeout);
    if (n > 0) {
        io_events_.fetch_add(n, std::memory_order_relaxed);
    }
#endif
}

void reactor::process_timers() noexcept {
    uint64_t now = now_ns();
    auto ready = timer_queue_->pop_ready(now);
    
    for (auto* t : ready) {
        t->run();
        timers_fired_.fetch_add(1, std::memory_order_relaxed);
        delete t;
    }
}

} // namespace core
} // namespace fasterapi

