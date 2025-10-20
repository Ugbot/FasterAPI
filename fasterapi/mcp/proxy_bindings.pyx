# cython: language_level=3
"""
Cython bindings for FasterAPI MCP Proxy (C++ backend)
"""

from libc.stdint cimport uint32_t, uint64_t
from libc.stdlib cimport malloc, free
from libc.string cimport strcpy

cdef extern from "mcp/mcp_lib.cpp":
    # Opaque handle type
    ctypedef void* MCPProxyHandle

    # Proxy creation/destruction
    MCPProxyHandle mcp_proxy_create(
        const char* name,
        const char* version,
        bint enable_auth,
        bint enable_rate_limiting,
        bint enable_authorization,
        bint enable_caching,
        uint32_t cache_ttl_ms,
        bint enable_request_logging,
        bint enable_metrics,
        bint failover_enabled,
        bint circuit_breaker_enabled,
        uint32_t circuit_breaker_threshold
    )

    void mcp_proxy_destroy(MCPProxyHandle handle)

    # Upstream management
    int mcp_proxy_add_upstream(
        MCPProxyHandle handle,
        const char* name,
        const char* transport_type,
        const char* command,
        int argc,
        const char** argv,
        const char* url,
        const char* auth_token,
        uint32_t max_connections,
        uint32_t connect_timeout_ms,
        uint32_t request_timeout_ms,
        bint enable_health_check,
        uint32_t health_check_interval_ms,
        uint32_t max_retries,
        uint32_t retry_delay_ms
    )

    # Route management
    int mcp_proxy_add_route(
        MCPProxyHandle handle,
        const char* upstream_name,
        const char* tool_pattern,
        const char* resource_pattern,
        const char* prompt_pattern,
        bint enable_request_transform,
        bint enable_response_transform,
        const char* required_scope,
        uint32_t rate_limit_override
    )

    # Request handling
    int mcp_proxy_handle_request(
        MCPProxyHandle handle,
        const char* request_json,
        const char* auth_header,
        char* response_buffer,
        size_t buffer_size
    )

    # Statistics
    int mcp_proxy_get_stats(
        MCPProxyHandle handle,
        char* stats_json,
        size_t buffer_size
    )

    # Health
    int mcp_proxy_get_upstream_health(
        MCPProxyHandle handle,
        char* health_json,
        size_t buffer_size
    )


cdef class ProxyBindings:
    """Cython wrapper for C++ MCP Proxy"""
    cdef MCPProxyHandle handle

    def __init__(
        self,
        str name,
        str version,
        bint enable_auth=True,
        bint enable_rate_limiting=True,
        bint enable_authorization=True,
        bint enable_caching=False,
        uint32_t cache_ttl_ms=60000,
        bint enable_request_logging=True,
        bint enable_metrics=True,
        bint failover_enabled=True,
        bint circuit_breaker_enabled=True,
        uint32_t circuit_breaker_threshold=5
    ):
        """Create proxy with C++ backend"""
        cdef bytes name_bytes = name.encode('utf-8')
        cdef bytes version_bytes = version.encode('utf-8')

        self.handle = mcp_proxy_create(
            name_bytes,
            version_bytes,
            enable_auth,
            enable_rate_limiting,
            enable_authorization,
            enable_caching,
            cache_ttl_ms,
            enable_request_logging,
            enable_metrics,
            failover_enabled,
            circuit_breaker_enabled,
            circuit_breaker_threshold
        )

        if self.handle == NULL:
            raise RuntimeError("Failed to create proxy")

    def __dealloc__(self):
        """Cleanup proxy"""
        if self.handle != NULL:
            mcp_proxy_destroy(self.handle)

    def add_upstream(
        self,
        str name,
        str transport_type,
        str command="",
        list args=None,
        str url="",
        str auth_token="",
        uint32_t max_connections=10,
        uint32_t connect_timeout_ms=5000,
        uint32_t request_timeout_ms=30000,
        bint enable_health_check=True,
        uint32_t health_check_interval_ms=30000,
        uint32_t max_retries=3,
        uint32_t retry_delay_ms=1000
    ):
        """Add upstream server"""
        cdef bytes name_bytes = name.encode('utf-8')
        cdef bytes transport_bytes = transport_type.encode('utf-8')
        cdef bytes command_bytes = command.encode('utf-8')
        cdef bytes url_bytes = url.encode('utf-8')
        cdef bytes auth_bytes = auth_token.encode('utf-8')

        # Convert args list to C array
        if args is None:
            args = []

        cdef int argc = len(args)
        cdef const char** argv = NULL
        cdef list args_bytes = []

        if argc > 0:
            argv = <const char**>malloc(argc * sizeof(char*))
            for i, arg in enumerate(args):
                arg_bytes = arg.encode('utf-8')
                args_bytes.append(arg_bytes)  # Keep reference
                argv[i] = arg_bytes

        try:
            result = mcp_proxy_add_upstream(
                self.handle,
                name_bytes,
                transport_bytes,
                command_bytes if command else NULL,
                argc,
                argv,
                url_bytes if url else NULL,
                auth_bytes if auth_token else NULL,
                max_connections,
                connect_timeout_ms,
                request_timeout_ms,
                enable_health_check,
                health_check_interval_ms,
                max_retries,
                retry_delay_ms
            )

            if result != 0:
                raise RuntimeError(f"Failed to add upstream: {name}")
        finally:
            if argv != NULL:
                free(argv)

    def add_route(
        self,
        str upstream_name,
        str tool_pattern="",
        str resource_pattern="",
        str prompt_pattern="",
        bint enable_request_transform=False,
        bint enable_response_transform=False,
        str required_scope=None,
        uint32_t rate_limit_override=0
    ):
        """Add routing rule"""
        cdef bytes upstream_bytes = upstream_name.encode('utf-8')
        cdef bytes tool_bytes = tool_pattern.encode('utf-8')
        cdef bytes resource_bytes = resource_pattern.encode('utf-8')
        cdef bytes prompt_bytes = prompt_pattern.encode('utf-8')
        cdef bytes scope_bytes
        cdef const char* scope_ptr = NULL

        if required_scope:
            scope_bytes = required_scope.encode('utf-8')
            scope_ptr = scope_bytes

        result = mcp_proxy_add_route(
            self.handle,
            upstream_bytes,
            tool_bytes if tool_pattern else NULL,
            resource_bytes if resource_pattern else NULL,
            prompt_bytes if prompt_pattern else NULL,
            enable_request_transform,
            enable_response_transform,
            scope_ptr,
            rate_limit_override
        )

        if result != 0:
            raise RuntimeError(f"Failed to add route to: {upstream_name}")

    def handle_request(self, str request_json, str auth_header=""):
        """Handle MCP request through proxy"""
        cdef bytes request_bytes = request_json.encode('utf-8')
        cdef bytes auth_bytes = auth_header.encode('utf-8')
        cdef char response_buffer[65536]  # 64KB buffer

        result = mcp_proxy_handle_request(
            self.handle,
            request_bytes,
            auth_bytes if auth_header else NULL,
            response_buffer,
            sizeof(response_buffer)
        )

        if result != 0:
            raise RuntimeError("Failed to handle request")

        return response_buffer.decode('utf-8')

    def get_stats(self):
        """Get proxy statistics as JSON"""
        cdef char stats_buffer[8192]  # 8KB buffer

        result = mcp_proxy_get_stats(
            self.handle,
            stats_buffer,
            sizeof(stats_buffer)
        )

        if result != 0:
            raise RuntimeError("Failed to get stats")

        return stats_buffer.decode('utf-8')

    def get_upstream_health(self):
        """Get upstream health status as JSON"""
        cdef char health_buffer[4096]  # 4KB buffer

        result = mcp_proxy_get_upstream_health(
            self.handle,
            health_buffer,
            sizeof(health_buffer)
        )

        if result != 0:
            raise RuntimeError("Failed to get upstream health")

        return health_buffer.decode('utf-8')
