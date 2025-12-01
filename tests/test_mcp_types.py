#!/usr/bin/env python3
"""
MCP Types Unit Tests

Tests the MCP type definitions (Tool, Resource, Prompt, etc.) and
proxy configuration dataclasses without requiring native bindings.

Tests cover:
- Tool dataclass
- Resource dataclass
- Prompt dataclass
- ToolResult dataclass
- ResourceContent dataclass
- TransportType enum
- UpstreamConfig dataclass
- ProxyRoute dataclass
- ProxyStats dataclass with computed properties
"""

import sys
import random
import string
from dataclasses import asdict

import pytest

sys.path.insert(0, '/Users/bengamble/FasterAPI')

# Import submodules directly using importlib to bypass broken __init__.py
import importlib.util
import types as types_module

def import_module_directly(module_path: str, module_name: str):
    """Import a module directly from file path, bypassing package __init__.py."""
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module

# Import types module directly
mcp_types = import_module_directly(
    '/Users/bengamble/FasterAPI/fasterapi/mcp/types.py',
    'fasterapi.mcp.types_direct'
)
Tool = mcp_types.Tool
Resource = mcp_types.Resource
Prompt = mcp_types.Prompt
ToolResult = mcp_types.ToolResult
ResourceContent = mcp_types.ResourceContent
TransportType = mcp_types.TransportType

# Mock the missing proxy_bindings to allow proxy import
mock_proxy_bindings = types_module.ModuleType('fasterapi.mcp.proxy_bindings')
mock_proxy_bindings.ProxyBindings = None
sys.modules['fasterapi.mcp.proxy_bindings'] = mock_proxy_bindings
sys.modules['.proxy_bindings'] = mock_proxy_bindings

# Import proxy module directly
mcp_proxy = import_module_directly(
    '/Users/bengamble/FasterAPI/fasterapi/mcp/proxy.py',
    'fasterapi.mcp.proxy_direct'
)
UpstreamConfig = mcp_proxy.UpstreamConfig
ProxyRoute = mcp_proxy.ProxyRoute
ProxyStats = mcp_proxy.ProxyStats


# =============================================================================
# Test Helpers
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate a random string."""
    first = random.choice(string.ascii_letters)
    if length == 1:
        return first
    rest = ''.join(random.choices(string.ascii_letters + string.digits, k=length - 1))
    return first + rest


def random_int(min_val: int = 1, max_val: int = 1000) -> int:
    """Generate a random integer."""
    return random.randint(min_val, max_val)


def random_uri() -> str:
    """Generate a random URI."""
    return f"resource://{random_string(8)}/{random_string(5)}"


# =============================================================================
# Tool Tests
# =============================================================================

class TestTool:
    """Tests for Tool dataclass."""

    def test_tool_creation(self):
        """Test basic tool creation."""
        tool = Tool(name="calculate", description="Perform calculations")
        assert tool.name == "calculate"
        assert tool.description == "Perform calculations"
        assert tool.input_schema is None

    def test_tool_with_schema(self):
        """Test tool with input schema."""
        schema = {
            "type": "object",
            "properties": {
                "operation": {"type": "string"},
                "a": {"type": "number"},
                "b": {"type": "number"}
            }
        }
        tool = Tool(name="math", description="Math operations", input_schema=schema)
        assert tool.input_schema == schema

    def test_tool_asdict(self):
        """Test tool conversion to dict."""
        tool = Tool(name="test", description="Test tool")
        d = asdict(tool)
        assert d['name'] == "test"
        assert d['description'] == "Test tool"

    def test_tool_randomized(self):
        """Test tool with randomized data."""
        name = random_string(15)
        desc = random_string(50)
        tool = Tool(name=name, description=desc)
        assert tool.name == name
        assert tool.description == desc


# =============================================================================
# Resource Tests
# =============================================================================

class TestResource:
    """Tests for Resource dataclass."""

    def test_resource_creation(self):
        """Test basic resource creation."""
        resource = Resource(uri="file:///config.json", name="Config")
        assert resource.uri == "file:///config.json"
        assert resource.name == "Config"
        assert resource.description is None
        assert resource.mime_type is None

    def test_resource_full(self):
        """Test resource with all fields."""
        resource = Resource(
            uri="file:///data.json",
            name="Data File",
            description="Application data",
            mime_type="application/json"
        )
        assert resource.description == "Application data"
        assert resource.mime_type == "application/json"

    def test_resource_asdict(self):
        """Test resource conversion to dict."""
        resource = Resource(uri="file:///test", name="Test")
        d = asdict(resource)
        assert 'uri' in d
        assert 'name' in d

    def test_resource_randomized(self):
        """Test resource with randomized data."""
        uri = random_uri()
        name = random_string(12)
        resource = Resource(uri=uri, name=name)
        assert resource.uri == uri
        assert resource.name == name


# =============================================================================
# Prompt Tests
# =============================================================================

class TestPrompt:
    """Tests for Prompt dataclass."""

    def test_prompt_creation(self):
        """Test basic prompt creation."""
        prompt = Prompt(name="summarize", description="Summarize text")
        assert prompt.name == "summarize"
        assert prompt.description == "Summarize text"
        assert prompt.arguments is None

    def test_prompt_with_arguments(self):
        """Test prompt with arguments."""
        prompt = Prompt(
            name="translate",
            description="Translate text",
            arguments=["text", "source_lang", "target_lang"]
        )
        assert prompt.arguments == ["text", "source_lang", "target_lang"]
        assert len(prompt.arguments) == 3

    def test_prompt_asdict(self):
        """Test prompt conversion to dict."""
        prompt = Prompt(name="test", description="Test prompt")
        d = asdict(prompt)
        assert d['name'] == "test"

    def test_prompt_randomized(self):
        """Test prompt with randomized data."""
        name = random_string(10)
        desc = random_string(30)
        num_args = random_int(1, 5)
        args = [random_string(8) for _ in range(num_args)]
        prompt = Prompt(name=name, description=desc, arguments=args)
        assert prompt.name == name
        assert len(prompt.arguments) == num_args


# =============================================================================
# ToolResult Tests
# =============================================================================

class TestToolResult:
    """Tests for ToolResult dataclass."""

    def test_tool_result_success(self):
        """Test successful tool result."""
        result = ToolResult(is_error=False, content='{"result": 42}')
        assert result.is_error is False
        assert result.content == '{"result": 42}'
        assert result.error_message is None

    def test_tool_result_error(self):
        """Test error tool result."""
        result = ToolResult(
            is_error=True,
            content="",
            error_message="Division by zero"
        )
        assert result.is_error is True
        assert result.error_message == "Division by zero"

    def test_tool_result_asdict(self):
        """Test tool result conversion to dict."""
        result = ToolResult(is_error=False, content="success")
        d = asdict(result)
        assert d['is_error'] is False
        assert d['content'] == "success"


# =============================================================================
# ResourceContent Tests
# =============================================================================

class TestResourceContent:
    """Tests for ResourceContent dataclass."""

    def test_resource_content_creation(self):
        """Test resource content creation."""
        content = ResourceContent(
            uri="file:///data.txt",
            mime_type="text/plain",
            content="Hello, World!"
        )
        assert content.uri == "file:///data.txt"
        assert content.mime_type == "text/plain"
        assert content.content == "Hello, World!"

    def test_resource_content_json(self):
        """Test resource content with JSON."""
        content = ResourceContent(
            uri="file:///config.json",
            mime_type="application/json",
            content='{"key": "value"}'
        )
        assert content.mime_type == "application/json"

    def test_resource_content_randomized(self):
        """Test resource content with randomized data."""
        uri = random_uri()
        mime = f"application/{random_string(5)}"
        text = random_string(100)
        content = ResourceContent(uri=uri, mime_type=mime, content=text)
        assert content.uri == uri
        assert content.content == text


# =============================================================================
# TransportType Tests
# =============================================================================

class TestTransportType:
    """Tests for TransportType enum."""

    def test_all_transport_types(self):
        """Test all transport types exist."""
        assert TransportType.STDIO.value == "stdio"
        assert TransportType.SSE.value == "sse"
        assert TransportType.STREAMABLE.value == "streamable"
        assert TransportType.WEBSOCKET.value == "websocket"

    def test_transport_type_from_string(self):
        """Test creating transport type from string."""
        assert TransportType("stdio") == TransportType.STDIO
        assert TransportType("websocket") == TransportType.WEBSOCKET

    def test_all_types_unique(self):
        """Test all transport types have unique values."""
        values = [t.value for t in TransportType]
        assert len(values) == len(set(values))


# =============================================================================
# UpstreamConfig Tests
# =============================================================================

class TestUpstreamConfig:
    """Tests for UpstreamConfig dataclass."""

    def test_upstream_stdio_config(self):
        """Test STDIO upstream configuration."""
        config = UpstreamConfig(
            name="math-server",
            transport_type="stdio",
            command="python",
            args=["math_server.py"]
        )
        assert config.name == "math-server"
        assert config.transport_type == "stdio"
        assert config.command == "python"
        assert config.args == ["math_server.py"]

    def test_upstream_http_config(self):
        """Test HTTP upstream configuration."""
        config = UpstreamConfig(
            name="api-server",
            transport_type="http",
            url="https://api.example.com/mcp",
            auth_token="secret-token"
        )
        assert config.transport_type == "http"
        assert config.url == "https://api.example.com/mcp"
        assert config.auth_token == "secret-token"

    def test_upstream_default_values(self):
        """Test upstream default values."""
        config = UpstreamConfig(name="test", transport_type="stdio")
        assert config.max_connections == 10
        assert config.connect_timeout_ms == 5000
        assert config.request_timeout_ms == 30000
        assert config.enable_health_check is True
        assert config.max_retries == 3

    def test_upstream_custom_timeouts(self):
        """Test upstream with custom timeouts."""
        config = UpstreamConfig(
            name="slow-server",
            transport_type="http",
            connect_timeout_ms=10000,
            request_timeout_ms=60000,
            max_retries=5
        )
        assert config.connect_timeout_ms == 10000
        assert config.request_timeout_ms == 60000
        assert config.max_retries == 5

    def test_upstream_asdict(self):
        """Test upstream conversion to dict."""
        config = UpstreamConfig(name="test", transport_type="stdio")
        d = asdict(config)
        assert d['name'] == "test"
        assert 'max_connections' in d


# =============================================================================
# ProxyRoute Tests
# =============================================================================

class TestProxyRoute:
    """Tests for ProxyRoute dataclass."""

    def test_proxy_route_basic(self):
        """Test basic proxy route."""
        route = ProxyRoute(upstream_name="math-server")
        assert route.upstream_name == "math-server"
        assert route.tool_pattern == ""
        assert route.resource_pattern == ""

    def test_proxy_route_with_patterns(self):
        """Test proxy route with patterns."""
        route = ProxyRoute(
            upstream_name="math-server",
            tool_pattern="math_*",
            resource_pattern="file://math/*"
        )
        assert route.tool_pattern == "math_*"
        assert route.resource_pattern == "file://math/*"

    def test_proxy_route_with_security(self):
        """Test proxy route with security settings."""
        route = ProxyRoute(
            upstream_name="admin-server",
            required_scope="admin",
            rate_limit_override=100
        )
        assert route.required_scope == "admin"
        assert route.rate_limit_override == 100

    def test_proxy_route_transforms(self):
        """Test proxy route with transforms enabled."""
        route = ProxyRoute(
            upstream_name="api-server",
            enable_request_transform=True,
            enable_response_transform=True
        )
        assert route.enable_request_transform is True
        assert route.enable_response_transform is True


# =============================================================================
# ProxyStats Tests
# =============================================================================

class TestProxyStats:
    """Tests for ProxyStats dataclass."""

    def test_proxy_stats_default(self):
        """Test default proxy stats."""
        stats = ProxyStats()
        assert stats.total_requests == 0
        assert stats.successful_requests == 0
        assert stats.failed_requests == 0

    def test_proxy_stats_with_values(self):
        """Test proxy stats with values."""
        stats = ProxyStats(
            total_requests=1000,
            successful_requests=950,
            failed_requests=50,
            total_latency_ms=50000
        )
        assert stats.total_requests == 1000
        assert stats.successful_requests == 950
        assert stats.failed_requests == 50

    def test_avg_latency_calculation(self):
        """Test average latency calculation."""
        stats = ProxyStats(
            total_requests=100,
            total_latency_ms=5000
        )
        assert stats.avg_latency_ms == 50.0

    def test_avg_latency_zero_requests(self):
        """Test average latency with zero requests."""
        stats = ProxyStats()
        assert stats.avg_latency_ms == 0.0

    def test_success_rate_calculation(self):
        """Test success rate calculation."""
        stats = ProxyStats(
            total_requests=100,
            successful_requests=95
        )
        assert stats.success_rate == 0.95

    def test_success_rate_zero_requests(self):
        """Test success rate with zero requests."""
        stats = ProxyStats()
        assert stats.success_rate == 0.0

    def test_success_rate_perfect(self):
        """Test perfect success rate."""
        stats = ProxyStats(
            total_requests=1000,
            successful_requests=1000
        )
        assert stats.success_rate == 1.0

    def test_success_rate_zero_success(self):
        """Test zero success rate."""
        stats = ProxyStats(
            total_requests=100,
            successful_requests=0
        )
        assert stats.success_rate == 0.0

    def test_proxy_stats_with_dict_fields(self):
        """Test proxy stats with dictionary fields."""
        stats = ProxyStats(
            total_requests=500,
            upstream_requests={'server1': 300, 'server2': 200},
            tool_requests={'calculate': 250, 'search': 150, 'other': 100}
        )
        assert stats.upstream_requests['server1'] == 300
        assert stats.tool_requests['calculate'] == 250

    def test_proxy_stats_randomized(self):
        """Test proxy stats with randomized values."""
        total = random_int(1000, 10000)
        success = random_int(0, total)
        failed = total - success
        latency = random_int(10000, 100000)

        stats = ProxyStats(
            total_requests=total,
            successful_requests=success,
            failed_requests=failed,
            total_latency_ms=latency
        )

        assert stats.success_rate == success / total
        assert stats.avg_latency_ms == latency / total


# =============================================================================
# Integration Tests
# =============================================================================

class TestMCPTypesIntegration:
    """Integration tests for MCP types working together."""

    def test_tool_collection(self):
        """Test creating a collection of tools."""
        tools = [
            Tool(name="add", description="Add numbers"),
            Tool(name="subtract", description="Subtract numbers"),
            Tool(name="multiply", description="Multiply numbers"),
            Tool(name="divide", description="Divide numbers"),
        ]

        assert len(tools) == 4
        tool_names = {t.name for t in tools}
        assert tool_names == {"add", "subtract", "multiply", "divide"}

    def test_resource_collection(self):
        """Test creating a collection of resources."""
        resources = [
            Resource(uri=f"file:///resource_{i}.json", name=f"Resource {i}")
            for i in range(10)
        ]

        assert len(resources) == 10
        assert all(r.uri.startswith("file://") for r in resources)

    def test_upstream_routing(self):
        """Test configuring upstreams and routes together."""
        upstreams = [
            UpstreamConfig(name="math", transport_type="stdio", command="python", args=["math.py"]),
            UpstreamConfig(name="search", transport_type="http", url="http://localhost:8001"),
            UpstreamConfig(name="files", transport_type="stdio", command="python", args=["files.py"]),
        ]

        routes = [
            ProxyRoute(upstream_name="math", tool_pattern="math_*"),
            ProxyRoute(upstream_name="search", tool_pattern="search_*"),
            ProxyRoute(upstream_name="files", resource_pattern="file://*"),
        ]

        # Verify routing config
        assert len(upstreams) == len(routes)
        upstream_names = {u.name for u in upstreams}
        route_targets = {r.upstream_name for r in routes}
        assert route_targets.issubset(upstream_names)

    def test_tool_result_from_tool(self):
        """Test creating tool result matching tool."""
        tool = Tool(name="greet", description="Greet user")
        result = ToolResult(
            is_error=False,
            content='{"message": "Hello!"}'
        )

        assert result.is_error is False
        assert "message" in result.content


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
