#include "sandbox.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

#ifdef __linux__
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#endif

namespace fasterapi {
namespace mcp {
namespace security {

Sandbox::Sandbox(const SandboxConfig& config)
    : config_(config)
{
}

uint64_t Sandbox::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void Sandbox::apply_limits() {
    struct rlimit limit;

    // CPU time limit
    if (config_.max_cpu_time_ms > 0) {
        limit.rlim_cur = limit.rlim_max = config_.max_cpu_time_ms / 1000;  // Convert to seconds
        setrlimit(RLIMIT_CPU, &limit);
    }

    // Memory limit (address space)
    if (config_.max_memory_bytes > 0) {
        limit.rlim_cur = limit.rlim_max = config_.max_memory_bytes;
        setrlimit(RLIMIT_AS, &limit);
    }

    // Stack size limit
    if (config_.max_stack_bytes > 0) {
        limit.rlim_cur = limit.rlim_max = config_.max_stack_bytes;
        setrlimit(RLIMIT_STACK, &limit);
    }

    // File size limit
    if (config_.max_file_size_bytes > 0) {
        limit.rlim_cur = limit.rlim_max = config_.max_file_size_bytes;
        setrlimit(RLIMIT_FSIZE, &limit);
    }

    // Open files limit
    if (config_.max_open_files > 0) {
        limit.rlim_cur = limit.rlim_max = config_.max_open_files;
        setrlimit(RLIMIT_NOFILE, &limit);
    }

    // Process limit (no forking)
    if (config_.max_processes > 0) {
        limit.rlim_cur = limit.rlim_max = config_.max_processes;
        setrlimit(RLIMIT_NPROC, &limit);
    }
}

void Sandbox::apply_seccomp_filter() {
#ifdef __linux__
    if (!config_.enable_seccomp) return;

    // Simple seccomp filter - allow minimal syscalls
    // In production, use libseccomp for easier filter creation

    // Enable no_new_privs to avoid privilege escalation
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    // Apply strict seccomp mode (only read, write, exit allowed)
    // For more complex filtering, use SECCOMP_MODE_FILTER with BPF
    prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT);
#endif
}

SandboxResult Sandbox::execute(ExecuteFunction func) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        return SandboxResult::fail("Failed to create pipe");
    }

    uint64_t start_time = now_ms();
    pid_t child_pid = fork();

    if (child_pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return SandboxResult::fail("Fork failed");
    }

    if (child_pid == 0) {
        // Child process
        close(pipe_fd[0]);  // Close read end

        // Apply resource limits
        apply_limits();

        // Apply seccomp filter (if enabled)
        // apply_seccomp_filter();  // Disabled for now - too restrictive

        try {
            // Execute function
            std::string result = func();

            // Write result to pipe
            write(pipe_fd[1], result.c_str(), result.length());
            close(pipe_fd[1]);

            _exit(0);
        } catch (const std::exception& e) {
            std::string error = std::string("Exception: ") + e.what();
            write(pipe_fd[1], error.c_str(), error.length());
            close(pipe_fd[1]);
            _exit(1);
        } catch (...) {
            const char* error = "Unknown exception";
            write(pipe_fd[1], error, strlen(error));
            close(pipe_fd[1]);
            _exit(1);
        }
    }

    // Parent process
    close(pipe_fd[1]);  // Close write end

    // Monitor child with timeout
    auto result = monitor_child(child_pid);

    // Read output from pipe
    char buffer[4096];
    ssize_t bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        result.output = std::string(buffer);
    }

    close(pipe_fd[0]);

    uint64_t execution_time = now_ms() - start_time;
    result.execution_time_ms = execution_time;

    return result;
}

SandboxResult Sandbox::monitor_child(pid_t child_pid) {
    uint64_t start_time = now_ms();
    bool timeout_occurred = false;

    while (true) {
        // Check for timeout
        uint64_t elapsed = now_ms() - start_time;
        if (config_.max_execution_time_ms > 0 && elapsed > config_.max_execution_time_ms) {
            // Kill child
            kill(child_pid, SIGKILL);
            timeout_occurred = true;
            break;
        }

        // Check if child exited
        int status;
        pid_t result = waitpid(child_pid, &status, WNOHANG);

        if (result == child_pid) {
            // Child exited
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    return SandboxResult::ok("", elapsed, 0);
                } else {
                    return SandboxResult::fail("Process exited with code " + std::to_string(exit_code), exit_code);
                }
            } else if (WIFSIGNALED(status)) {
                int signal = WTERMSIG(status);
                return SandboxResult::fail("Process terminated by signal " + std::to_string(signal), -signal);
            }
        } else if (result < 0) {
            return SandboxResult::fail("waitpid failed");
        }

        // Sleep briefly before next check
        usleep(1000);  // 1ms
    }

    if (timeout_occurred) {
        waitpid(child_pid, nullptr, 0);  // Reap zombie
        return SandboxResult::fail("Execution timeout exceeded");
    }

    return SandboxResult::fail("Unknown error");
}

SandboxResult Sandbox::execute_command(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& input
) {
    return execute([&]() -> std::string {
        // Create pipes for stdin/stdout
        int stdin_pipe[2];
        int stdout_pipe[2];

        if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
            throw std::runtime_error("Failed to create pipes");
        }

        pid_t pid = fork();
        if (pid < 0) {
            throw std::runtime_error("Fork failed");
        }

        if (pid == 0) {
            // Child: execute command

            // Redirect stdin/stdout
            dup2(stdin_pipe[0], STDIN_FILENO);
            dup2(stdout_pipe[1], STDOUT_FILENO);

            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);

            // Build argv
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(command.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            // Execute
            execvp(command.c_str(), argv.data());

            // If we get here, exec failed
            _exit(127);
        }

        // Parent: write input and read output
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        // Write input
        if (!input.empty()) {
            write(stdin_pipe[1], input.c_str(), input.length());
        }
        close(stdin_pipe[1]);

        // Read output
        std::string output;
        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = read(stdout_pipe[0], buffer, sizeof(buffer))) > 0) {
            output.append(buffer, bytes_read);
        }
        close(stdout_pipe[0]);

        // Wait for child
        int status;
        waitpid(pid, &status, 0);

        return output;
    });
}

// ========== ToolSandbox ==========

ToolSandbox::ToolSandbox(const Config& config)
    : config_(config)
    , sandbox_(config.sandbox_config)
{
}

SandboxResult ToolSandbox::execute_tool(const std::string& tool_name, Sandbox::ExecuteFunction func) {
    auto result = sandbox_.execute(func);

    if (config_.collect_metrics) {
        record_execution(tool_name, result);
    }

    return result;
}

void ToolSandbox::record_execution(const std::string& tool_name, const SandboxResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& metrics = metrics_[tool_name];
    metrics.executions++;

    if (!result.success) {
        metrics.failures++;
    }

    metrics.total_execution_time_ms += result.execution_time_ms;
    metrics.max_execution_time_ms = std::max(metrics.max_execution_time_ms, result.execution_time_ms);

    metrics.total_memory_bytes += result.memory_used_bytes;
    metrics.max_memory_bytes = std::max(metrics.max_memory_bytes, result.memory_used_bytes);
}

std::string ToolSandbox::get_metrics(const std::string& tool_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = metrics_.find(tool_name);
    if (it == metrics_.end()) {
        return "{}";
    }

    const auto& m = it->second;

    std::ostringstream oss;
    oss << "{"
        << "\"executions\":" << m.executions << ","
        << "\"failures\":" << m.failures << ","
        << "\"success_rate\":" << (m.executions > 0 ? (1.0 - (double)m.failures / m.executions) : 0) << ","
        << "\"total_execution_time_ms\":" << m.total_execution_time_ms << ","
        << "\"avg_execution_time_ms\":" << (m.executions > 0 ? m.total_execution_time_ms / m.executions : 0) << ","
        << "\"max_execution_time_ms\":" << m.max_execution_time_ms << ","
        << "\"max_memory_bytes\":" << m.max_memory_bytes
        << "}";

    return oss.str();
}

} // namespace security
} // namespace mcp
} // namespace fasterapi
