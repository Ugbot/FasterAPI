#pragma once

#include <string>
#include <functional>
#include <chrono>
#include <optional>
#include <unistd.h>
#include <sys/resource.h>

namespace fasterapi {
namespace mcp {
namespace security {

/**
 * Sandbox execution result
 */
struct SandboxResult {
    bool success;
    std::string output;
    std::string error_message;
    int exit_code;
    uint64_t execution_time_ms;
    uint64_t memory_used_bytes;

    static SandboxResult ok(const std::string& output, uint64_t exec_time_ms, uint64_t memory_bytes) {
        return SandboxResult{true, output, "", 0, exec_time_ms, memory_bytes};
    }

    static SandboxResult fail(const std::string& error, int exit_code = -1) {
        return SandboxResult{false, "", error, exit_code, 0, 0};
    }
};

/**
 * Sandbox configuration
 */
struct SandboxConfig {
    // Time limits
    uint64_t max_execution_time_ms = 5000;  // 5 seconds
    uint64_t max_cpu_time_ms = 5000;        // 5 seconds

    // Memory limits
    uint64_t max_memory_bytes = 100 * 1024 * 1024;  // 100 MB
    uint64_t max_stack_bytes = 8 * 1024 * 1024;     // 8 MB

    // File system limits
    uint64_t max_file_size_bytes = 10 * 1024 * 1024;  // 10 MB
    uint64_t max_open_files = 64;

    // Process limits
    uint64_t max_processes = 1;  // No forking

    // Network
    bool allow_network = false;

    // File system access
    bool allow_file_read = true;
    bool allow_file_write = false;
    std::vector<std::string> allowed_paths;  // Whitelist of paths

    // System calls
    bool enable_seccomp = false;  // Enable seccomp filtering (Linux only)
};

/**
 * Sandbox for executing untrusted code.
 *
 * Uses multiple layers of isolation:
 * - Process isolation (fork)
 * - Resource limits (setrlimit)
 * - Timeout enforcement
 * - Optional: seccomp syscall filtering (Linux)
 * - Optional: chroot jail
 */
class Sandbox {
public:
    using ExecuteFunction = std::function<std::string()>;

    explicit Sandbox(const SandboxConfig& config = SandboxConfig());

    /**
     * Execute a function in a sandboxed environment.
     *
     * @param func Function to execute
     * @return Execution result
     */
    SandboxResult execute(ExecuteFunction func);

    /**
     * Execute a shell command in a sandboxed environment.
     *
     * @param command Command to execute
     * @param args Command arguments
     * @param input Standard input data
     * @return Execution result
     */
    SandboxResult execute_command(
        const std::string& command,
        const std::vector<std::string>& args = {},
        const std::string& input = ""
    );

private:
    SandboxConfig config_;

    // Apply resource limits in child process
    void apply_limits();

    // Apply seccomp filter (Linux only)
    void apply_seccomp_filter();

    // Monitor child process
    SandboxResult monitor_child(pid_t child_pid);

    // Kill child process if timeout exceeded
    void enforce_timeout(pid_t child_pid, uint64_t start_time_ms);

    // Get current time in milliseconds
    static uint64_t now_ms();
};

/**
 * Tool execution sandbox.
 *
 * Specialized sandbox for MCP tool execution with:
 * - Per-tool resource limits
 * - Execution metrics
 * - Automatic cleanup
 */
class ToolSandbox {
public:
    struct Config {
        SandboxConfig sandbox_config;
        bool log_execution = true;
        bool collect_metrics = true;
    };

    explicit ToolSandbox(const Config& config = Config());

    /**
     * Execute a tool in sandbox.
     *
     * @param tool_name Tool name (for logging)
     * @param func Tool function
     * @return Execution result
     */
    SandboxResult execute_tool(const std::string& tool_name, Sandbox::ExecuteFunction func);

    /**
     * Get execution metrics for a tool.
     *
     * @param tool_name Tool name
     * @return Metrics (JSON)
     */
    std::string get_metrics(const std::string& tool_name);

private:
    Config config_;
    Sandbox sandbox_;

    struct ToolMetrics {
        uint64_t executions = 0;
        uint64_t failures = 0;
        uint64_t total_execution_time_ms = 0;
        uint64_t max_execution_time_ms = 0;
        uint64_t total_memory_bytes = 0;
        uint64_t max_memory_bytes = 0;
    };

    std::unordered_map<std::string, ToolMetrics> metrics_;
    mutable std::mutex mutex_;

    void record_execution(const std::string& tool_name, const SandboxResult& result);
};

} // namespace security
} // namespace mcp
} // namespace fasterapi
