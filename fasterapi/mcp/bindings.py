"""
C bindings for MCP native library.
"""

import ctypes
import os
from pathlib import Path


# Find the native library
def find_library():
    """Find the MCP native library."""
    # Try relative to this file
    lib_dir = Path(__file__).parent.parent / "_native"

    # Common library names by platform
    import sys

    if sys.platform == "darwin":
        lib_names = ["libfasterapi_mcp.dylib"]
    elif sys.platform.startswith("linux"):
        lib_names = ["libfasterapi_mcp.so"]
    elif sys.platform == "win32":
        lib_names = ["fasterapi_mcp.dll", "libfasterapi_mcp.dll"]
    else:
        lib_names = ["libfasterapi_mcp.so"]

    for lib_name in lib_names:
        lib_path = lib_dir / lib_name
        if lib_path.exists():
            return str(lib_path)

    raise RuntimeError(f"MCP native library not found in {lib_dir}")


# Load library
try:
    _lib = ctypes.CDLL(find_library())
except Exception as e:
    # Library not built yet - use stubs
    _lib = None


class MCPBindings:
    """Low-level bindings to C API."""

    # Opaque handle types
    MCPServerHandle = ctypes.c_void_p
    MCPClientHandle = ctypes.c_void_p
    TransportHandle = ctypes.c_void_p

    def __init__(self):
        if _lib is None:
            raise RuntimeError(
                "MCP native library not loaded. Run 'make build' to compile."
            )

        self._lib = _lib
        self._setup_functions()

    def _setup_functions(self):
        """Setup function signatures."""

        # Server functions
        self._lib.mcp_server_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self._lib.mcp_server_create.restype = self.MCPServerHandle

        self._lib.mcp_server_destroy.argtypes = [self.MCPServerHandle]
        self._lib.mcp_server_destroy.restype = None

        self._lib.mcp_server_start_stdio.argtypes = [self.MCPServerHandle]
        self._lib.mcp_server_start_stdio.restype = ctypes.c_int

        self._lib.mcp_server_stop.argtypes = [self.MCPServerHandle]
        self._lib.mcp_server_stop.restype = None

        self._lib.mcp_server_register_tool.argtypes = [
            self.MCPServerHandle,
            ctypes.c_char_p,  # name
            ctypes.c_char_p,  # description
            ctypes.c_char_p,  # input_schema
            ctypes.c_uint64,  # handler_id
        ]
        self._lib.mcp_server_register_tool.restype = ctypes.c_int

        self._lib.mcp_server_register_resource.argtypes = [
            self.MCPServerHandle,
            ctypes.c_char_p,  # uri
            ctypes.c_char_p,  # name
            ctypes.c_char_p,  # description
            ctypes.c_char_p,  # mime_type
            ctypes.c_uint64,  # provider_id
        ]
        self._lib.mcp_server_register_resource.restype = ctypes.c_int

        self._lib.mcp_server_start_sse.argtypes = [
            self.MCPServerHandle,
            ctypes.c_char_p,  # host
            ctypes.c_uint16,  # port
        ]
        self._lib.mcp_server_start_sse.restype = ctypes.c_int

        self._lib.mcp_server_start_websocket.argtypes = [
            self.MCPServerHandle,
            ctypes.c_char_p,  # host
            ctypes.c_uint16,  # port
        ]
        self._lib.mcp_server_start_websocket.restype = ctypes.c_int

        # Client functions
        self._lib.mcp_client_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self._lib.mcp_client_create.restype = self.MCPClientHandle

        self._lib.mcp_client_destroy.argtypes = [self.MCPClientHandle]
        self._lib.mcp_client_destroy.restype = None

        self._lib.mcp_client_connect_stdio.argtypes = [
            self.MCPClientHandle,
            ctypes.c_char_p,  # command
            ctypes.c_int,  # argc
            ctypes.POINTER(ctypes.c_char_p),  # argv
        ]
        self._lib.mcp_client_connect_stdio.restype = ctypes.c_int

        self._lib.mcp_client_disconnect.argtypes = [self.MCPClientHandle]
        self._lib.mcp_client_disconnect.restype = None

        self._lib.mcp_client_call_tool.argtypes = [
            self.MCPClientHandle,
            ctypes.c_char_p,  # name
            ctypes.c_char_p,  # params
            ctypes.c_char_p,  # result_buffer
            ctypes.c_size_t,  # buffer_size
        ]
        self._lib.mcp_client_call_tool.restype = ctypes.c_int

        self._lib.mcp_client_list_tools.argtypes = [
            self.MCPClientHandle,
            ctypes.c_char_p,  # result_buffer
            ctypes.c_size_t,  # buffer_size
        ]
        self._lib.mcp_client_list_tools.restype = ctypes.c_int

        self._lib.mcp_client_list_resources.argtypes = [
            self.MCPClientHandle,
            ctypes.c_char_p,  # result_buffer
            ctypes.c_size_t,  # buffer_size
        ]
        self._lib.mcp_client_list_resources.restype = ctypes.c_int

        self._lib.mcp_client_read_resource.argtypes = [
            self.MCPClientHandle,
            ctypes.c_char_p,  # uri
            ctypes.c_char_p,  # result_buffer
            ctypes.c_size_t,  # buffer_size
        ]
        self._lib.mcp_client_read_resource.restype = ctypes.c_int

        self._lib.mcp_client_list_prompts.argtypes = [
            self.MCPClientHandle,
            ctypes.c_char_p,  # result_buffer
            ctypes.c_size_t,  # buffer_size
        ]
        self._lib.mcp_client_list_prompts.restype = ctypes.c_int

        self._lib.mcp_client_get_prompt.argtypes = [
            self.MCPClientHandle,
            ctypes.c_char_p,  # name
            ctypes.c_char_p,  # args_json
            ctypes.c_char_p,  # result_buffer
            ctypes.c_size_t,  # buffer_size
        ]
        self._lib.mcp_client_get_prompt.restype = ctypes.c_int

    # Server methods
    def server_create(self, name: str, version: str):
        return self._lib.mcp_server_create(
            name.encode("utf-8"), version.encode("utf-8")
        )

    def server_destroy(self, handle):
        self._lib.mcp_server_destroy(handle)

    def server_start_stdio(self, handle):
        return self._lib.mcp_server_start_stdio(handle)

    def server_stop(self, handle):
        self._lib.mcp_server_stop(handle)

    def server_register_tool(
        self, handle, name: str, description: str, input_schema: str, handler_id: int
    ):
        return self._lib.mcp_server_register_tool(
            handle,
            name.encode("utf-8"),
            description.encode("utf-8"),
            input_schema.encode("utf-8") if input_schema else b"",
            handler_id,
        )

    def server_register_resource(
        self,
        handle,
        uri: str,
        name: str,
        description: str,
        mime_type: str,
        provider_id: int,
    ):
        return self._lib.mcp_server_register_resource(
            handle,
            uri.encode("utf-8"),
            name.encode("utf-8"),
            description.encode("utf-8") if description else b"",
            mime_type.encode("utf-8") if mime_type else b"",
            provider_id,
        )

    def server_start_sse(self, handle, host: str, port: int):
        return self._lib.mcp_server_start_sse(handle, host.encode("utf-8"), port)

    def server_start_websocket(self, handle, host: str, port: int):
        return self._lib.mcp_server_start_websocket(handle, host.encode("utf-8"), port)

    # Client methods
    def client_create(self, name: str, version: str):
        return self._lib.mcp_client_create(
            name.encode("utf-8"), version.encode("utf-8")
        )

    def client_destroy(self, handle):
        self._lib.mcp_client_destroy(handle)

    def client_connect_stdio(self, handle, command: str, args: list):
        argc = len(args)
        argv = (ctypes.c_char_p * argc)()
        for i, arg in enumerate(args):
            argv[i] = arg.encode("utf-8")

        return self._lib.mcp_client_connect_stdio(
            handle, command.encode("utf-8"), argc, argv
        )

    def client_disconnect(self, handle):
        self._lib.mcp_client_disconnect(handle)

    def client_call_tool(self, handle, name: str, params: str) -> str:
        buffer_size = 65536  # 64KB
        result_buffer = ctypes.create_string_buffer(buffer_size)

        result = self._lib.mcp_client_call_tool(
            handle,
            name.encode("utf-8"),
            params.encode("utf-8"),
            result_buffer,
            buffer_size,
        )

        if result != 0:
            raise RuntimeError("Tool call failed")

        return result_buffer.value.decode("utf-8")

    def client_list_tools(self, handle) -> str:
        buffer_size = 65536
        result_buffer = ctypes.create_string_buffer(buffer_size)

        result = self._lib.mcp_client_list_tools(handle, result_buffer, buffer_size)

        if result != 0:
            return ""

        return result_buffer.value.decode("utf-8")

    def client_list_resources(self, handle) -> str:
        buffer_size = 65536
        result_buffer = ctypes.create_string_buffer(buffer_size)

        result = self._lib.mcp_client_list_resources(handle, result_buffer, buffer_size)

        if result != 0:
            return ""

        return result_buffer.value.decode("utf-8")

    def client_read_resource(self, handle, uri: str) -> str:
        buffer_size = 65536
        result_buffer = ctypes.create_string_buffer(buffer_size)

        result = self._lib.mcp_client_read_resource(
            handle, uri.encode("utf-8"), result_buffer, buffer_size
        )

        if result != 0:
            return ""

        return result_buffer.value.decode("utf-8")

    def client_list_prompts(self, handle) -> str:
        buffer_size = 65536
        result_buffer = ctypes.create_string_buffer(buffer_size)

        result = self._lib.mcp_client_list_prompts(handle, result_buffer, buffer_size)

        if result != 0:
            return ""

        return result_buffer.value.decode("utf-8")

    def client_get_prompt(self, handle, name: str, args_json: str) -> str:
        buffer_size = 65536
        result_buffer = ctypes.create_string_buffer(buffer_size)

        result = self._lib.mcp_client_get_prompt(
            handle,
            name.encode("utf-8"),
            args_json.encode("utf-8"),
            result_buffer,
            buffer_size,
        )

        if result != 0:
            return ""

        return result_buffer.value.decode("utf-8")


# Global bindings instance
try:
    bindings = MCPBindings()
except Exception:
    bindings = None  # Not built yet
