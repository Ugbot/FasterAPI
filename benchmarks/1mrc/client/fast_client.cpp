/*
 * Fast C++ HTTP/1.1 benchmark client
 * Tests server throughput without Python bottleneck
 */

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

using namespace std;
using namespace chrono;

// Global counters
atomic<uint64_t> g_requests_sent{0};
atomic<uint64_t> g_requests_completed{0};
atomic<uint64_t> g_errors{0};

// Test configuration
struct Config {
    string host = "127.0.0.1";
    int port = 8000;
    int total_requests = 100000;
    int num_threads = 16;
    int connections_per_thread = 10;
};

// Create socket and connect
int connect_to_server(const string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    // Disable Nagle's algorithm
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

// Send GET request for /stats
bool send_stats_request(int sock) {
    const char* request =
        "GET /stats HTTP/1.1\r\n"
        "Host: localhost:8000\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    ssize_t sent = send(sock, request, strlen(request), 0);
    if (sent <= 0) {
        return false;
    }

    // Read response (simple version - just consume it)
    char buffer[4096];
    ssize_t received = recv(sock, buffer, sizeof(buffer), 0);

    return received > 0;
}

// Worker thread
void worker_thread(const Config& config, int thread_id) {
    int requests_per_thread = config.total_requests / config.num_threads;

    // Create multiple persistent connections
    vector<int> sockets;
    for (int i = 0; i < config.connections_per_thread; i++) {
        int sock = connect_to_server(config.host, config.port);
        if (sock >= 0) {
            sockets.push_back(sock);
        } else {
            cerr << "Thread " << thread_id << ": Failed to create connection " << i << endl;
        }
    }

    if (sockets.empty()) {
        cerr << "Thread " << thread_id << ": No connections available!" << endl;
        return;
    }

    // Send requests round-robin across connections
    for (int i = 0; i < requests_per_thread; i++) {
        int sock = sockets[i % sockets.size()];

        if (send_stats_request(sock)) {
            g_requests_completed.fetch_add(1, memory_order_relaxed);
        } else {
            g_errors.fetch_add(1, memory_order_relaxed);
        }

        g_requests_sent.fetch_add(1, memory_order_relaxed);
    }

    // Close all connections
    for (int sock : sockets) {
        close(sock);
    }
}

int main(int argc, char* argv[]) {
    Config config;

    if (argc > 1) config.total_requests = atoi(argv[1]);
    if (argc > 2) config.num_threads = atoi(argv[2]);

    cout << "====================================================\n";
    cout << "Fast C++ HTTP/1.1 Benchmark Client\n";
    cout << "====================================================\n";
    cout << "Server:           " << config.host << ":" << config.port << "\n";
    cout << "Total requests:   " << config.total_requests << "\n";
    cout << "Threads:          " << config.num_threads << "\n";
    cout << "Conn/thread:      " << config.connections_per_thread << "\n";
    cout << "Total connections:" << (config.num_threads * config.connections_per_thread) << "\n";
    cout << "====================================================\n\n";

    // Start timing
    auto start = high_resolution_clock::now();

    // Launch worker threads
    vector<thread> threads;
    for (int i = 0; i < config.num_threads; i++) {
        threads.emplace_back(worker_thread, ref(config), i);
    }

    // Progress reporting thread
    thread progress([&]() {
        while (g_requests_sent.load() < config.total_requests) {
            this_thread::sleep_for(milliseconds(1000));
            auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start).count() / 1000.0;
            uint64_t completed = g_requests_completed.load();
            double rps = completed / elapsed;
            printf("Progress: %lu/%d (%.1f req/s)\n", completed, config.total_requests, rps);
        }
    });

    // Wait for all workers
    for (auto& t : threads) {
        t.join();
    }

    // Stop progress thread
    progress.join();

    // Final timing
    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();
    double duration_s = duration_ms / 1000.0;

    uint64_t completed = g_requests_completed.load();
    uint64_t errors = g_errors.load();
    double rps = completed / duration_s;

    cout << "\n====================================================\n";
    cout << "Results\n";
    cout << "====================================================\n";
    cout << "Total time:       " << duration_s << "s\n";
    cout << "Completed:        " << completed << "\n";
    cout << "Errors:           " << errors << "\n";
    cout << "Requests/sec:     " << rps << "\n";
    cout << "====================================================\n";

    return 0;
}
