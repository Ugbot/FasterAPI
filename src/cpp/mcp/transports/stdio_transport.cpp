#include "stdio_transport.h"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <iostream>
#include <cstring>

namespace fasterapi {
namespace mcp {

StdioTransport::StdioTransport()
    : is_server_mode_(true)
{
    stdin_fd_ = STDIN_FILENO;
    stdout_fd_ = STDOUT_FILENO;
}

StdioTransport::StdioTransport(const std::string& command, const std::vector<std::string>& args)
    : is_server_mode_(false)
    , command_(command)
    , args_(args)
{
}

StdioTransport::~StdioTransport() {
    disconnect();
}

int StdioTransport::connect() {
    if (state_ != TransportState::DISCONNECTED) {
        return -1;  // Already connected
    }

    set_state(TransportState::CONNECTING);

    if (!is_server_mode_) {
        // Launch subprocess
        int result = launch_subprocess();
        if (result != 0) {
            set_state(TransportState::ERROR);
            return result;
        }
    }

    // Start reader thread
    reader_running_ = true;
    reader_thread_ = std::make_unique<std::thread>(&StdioTransport::reader_loop, this);

    set_state(TransportState::CONNECTED);
    return 0;
}

int StdioTransport::disconnect() {
    if (state_ == TransportState::DISCONNECTED) {
        return 0;
    }

    set_state(TransportState::DISCONNECTING);

    // Stop reader thread
    reader_running_ = false;
    if (reader_thread_ && reader_thread_->joinable()) {
        reader_thread_->join();
    }

    // Close subprocess if client mode
    if (!is_server_mode_) {
        close_subprocess();
    }

    set_state(TransportState::DISCONNECTED);
    return 0;
}

int StdioTransport::send(const std::string& message) {
    if (!is_connected()) {
        return -1;
    }

    // Messages are newline-delimited
    std::string line = message + "\n";
    return write_line(stdout_fd_, line);
}

std::optional<std::string> StdioTransport::receive(uint32_t timeout_ms) {
    // Lock-free queue with spinning/polling
    std::string message;

    if (timeout_ms == 0) {
        // Wait indefinitely - poll with short sleeps
        while (reader_running_) {
            if (message_queue_.try_pop(message)) {
                return message;
            }
            // Yield to avoid busy-wait burning CPU
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    } else {
        // Wait with timeout
        auto start = std::chrono::steady_clock::now();
        auto deadline = start + std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            if (message_queue_.try_pop(message)) {
                return message;
            }
            // Short sleep to avoid busy-wait
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return std::nullopt;  // Timeout
    }

    return std::nullopt;
}

void StdioTransport::set_message_callback(MessageCallback callback) {
    message_callback_ = callback;
}

void StdioTransport::set_error_callback(ErrorCallback callback) {
    error_callback_ = callback;
}

void StdioTransport::set_state_callback(StateCallback callback) {
    state_callback_ = callback;
}

TransportState StdioTransport::get_state() const {
    return state_.load();
}

bool StdioTransport::is_connected() const {
    return state_ == TransportState::CONNECTED;
}

void StdioTransport::reader_loop() {
    while (reader_running_) {
        auto message = read_line(stdin_fd_, 100);  // 100ms timeout
        if (message.has_value()) {
            // Got a message
            if (message_callback_) {
                // Async callback
                message_callback_(message.value());
            } else {
                // Queue for synchronous receive() - lock-free!
                while (!message_queue_.try_push(message.value())) {
                    // Queue full - extremely rare with 16K capacity
                    // Yield and retry
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            }
        }
    }
}

void StdioTransport::set_state(TransportState new_state) {
    TransportState old_state = state_.exchange(new_state);
    if (old_state != new_state && state_callback_) {
        state_callback_(new_state);
    }
}

void StdioTransport::invoke_error(const std::string& error) {
    if (error_callback_) {
        error_callback_(error);
    }
    set_state(TransportState::ERROR);
}

int StdioTransport::launch_subprocess() {
    // Create pipes for stdin/stdout
    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        invoke_error("Failed to create pipes: " + std::string(strerror(errno)));
        return -1;
    }

    // Fork
    child_pid_ = fork();
    if (child_pid_ < 0) {
        invoke_error("Fork failed: " + std::string(strerror(errno)));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    if (child_pid_ == 0) {
        // Child process

        // Redirect stdin
        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);

        // Redirect stdout
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        // Build argv
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(command_.c_str()));
        for (const auto& arg : args_) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // Execute
        execvp(command_.c_str(), argv.data());

        // If execvp returns, it failed
        std::cerr << "execvp failed: " << strerror(errno) << std::endl;
        _exit(1);
    }

    // Parent process
    close(stdin_pipe[0]);   // Close read end of stdin pipe
    close(stdout_pipe[1]);  // Close write end of stdout pipe

    stdin_fd_ = stdout_pipe[0];   // We read from child's stdout
    stdout_fd_ = stdin_pipe[1];   // We write to child's stdin

    // Set non-blocking mode for reading
    fcntl(stdin_fd_, F_SETFL, fcntl(stdin_fd_, F_GETFL) | O_NONBLOCK);

    return 0;
}

int StdioTransport::close_subprocess() {
    if (child_pid_ > 0) {
        // Close file descriptors
        if (stdin_fd_ >= 0) {
            close(stdin_fd_);
            stdin_fd_ = -1;
        }
        if (stdout_fd_ >= 0) {
            close(stdout_fd_);
            stdout_fd_ = -1;
        }

        // Send SIGTERM and wait
        kill(child_pid_, SIGTERM);

        int status;
        int wait_result = waitpid(child_pid_, &status, WNOHANG);

        if (wait_result == 0) {
            // Not dead yet, wait a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            wait_result = waitpid(child_pid_, &status, WNOHANG);

            if (wait_result == 0) {
                // Still not dead, force kill
                kill(child_pid_, SIGKILL);
                waitpid(child_pid_, &status, 0);
            }
        }

        child_pid_ = -1;
    }

    return 0;
}

std::optional<std::string> StdioTransport::read_line(int fd, uint32_t timeout_ms) {
    static std::string buffer;  // Persistent buffer across calls

    // Use poll to wait for data with timeout
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result < 0) {
        if (errno != EINTR) {
            invoke_error("Poll error: " + std::string(strerror(errno)));
        }
        return std::nullopt;
    }
    if (poll_result == 0) {
        // Timeout
        return std::nullopt;
    }

    // Read data
    char temp_buf[4096];
    ssize_t bytes_read = read(fd, temp_buf, sizeof(temp_buf) - 1);

    if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            invoke_error("Read error: " + std::string(strerror(errno)));
        }
        return std::nullopt;
    }

    if (bytes_read == 0) {
        // EOF
        invoke_error("EOF on stdin");
        return std::nullopt;
    }

    // Append to buffer
    temp_buf[bytes_read] = '\0';
    buffer += temp_buf;

    // Check for newline
    auto newline_pos = buffer.find('\n');
    if (newline_pos != std::string::npos) {
        std::string line = buffer.substr(0, newline_pos);
        buffer = buffer.substr(newline_pos + 1);

        // Trim whitespace
        while (!line.empty() && std::isspace(line.back())) {
            line.pop_back();
        }

        return line;
    }

    return std::nullopt;
}

int StdioTransport::write_line(int fd, const std::string& line) {
    ssize_t bytes_written = write(fd, line.c_str(), line.length());
    if (bytes_written < 0) {
        invoke_error("Write error: " + std::string(strerror(errno)));
        return -1;
    }
    if (static_cast<size_t>(bytes_written) != line.length()) {
        invoke_error("Partial write");
        return -1;
    }

    // Flush
    fsync(fd);

    return 0;
}

// Factory method
std::unique_ptr<Transport> TransportFactory::create_stdio(
    const std::string& command,
    const std::vector<std::string>& args
) {
    if (command.empty()) {
        return std::make_unique<StdioTransport>();  // Server mode
    } else {
        return std::make_unique<StdioTransport>(command, args);  // Client mode
    }
}

} // namespace mcp
} // namespace fasterapi
