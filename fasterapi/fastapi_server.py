"""
FastAPI-compatible HTTP server integration.

This module bridges the FastAPI compatibility layer with the high-performance
C++ HTTP server, providing parameter extraction, validation, and automatic
documentation generation.
"""

import json
import inspect
from typing import Any, Callable, Dict, Optional
try:
    from fasterapi.http.server_cy import Server
except ImportError:
    # Fall back to ctypes-based server if Cython extension not available
    from fasterapi.http.server import Server
from fasterapi.fastapi_compat import FastAPIApp

try:
    from fasterapi._fastapi_native import (
        get_all_routes,
        generate_openapi,
        generate_swagger_ui_response,
        generate_redoc_response,
        connect_route_registry_to_server
    )
    HAS_NATIVE = True
except ImportError:
    HAS_NATIVE = False
    print("Warning: Native bindings not available")
    connect_route_registry_to_server = None


class FastAPIServer(Server):
    """
    FastAPI-compatible HTTP server with C++ performance.

    Integrates the FastAPI decorator layer with the high-performance C++ server,
    providing automatic parameter extraction, validation, and documentation.
    """

    def __init__(self, app: FastAPIApp, **kwargs):
        """
        Create FastAPI server.

        Args:
            app: FastAPIApp instance with registered routes
            **kwargs: Server configuration (port, host, etc.)
        """
        self.app = app

        # CRITICAL: Connect RouteRegistry BEFORE creating server
        # This must happen before super().__init__() which syncs routes
        if HAS_NATIVE and connect_route_registry_to_server is not None:
            connect_route_registry_to_server()

        # Now create the server (parent __init__ will sync routes from RouteRegistry)
        super().__init__(**kwargs)

        # Routes are automatically synced from RouteRegistry during start()
        # The parent Server class handles this via _sync_routes_from_registry()


def run_fastapi_app(app: FastAPIApp,
                    host: str = "0.0.0.0",
                    port: int = 8000,
                    **kwargs):
    """
    Run a FastAPI application.

    Args:
        app: FastAPIApp instance
        host: Server host
        port: Server port
        **kwargs: Additional server configuration
    """
    # Create server
    server = FastAPIServer(app, host=host, port=port, **kwargs)

    # Start server
    server.start()

    print("\n" + "="*80)
    print(f"FastAPI server running at http://{host}:{port}")
    print("="*80)
    print(f"  📚 Documentation: http://{host}:{port}{app.docs_url}")
    print(f"  📖 ReDoc:         http://{host}:{port}{app.redoc_url}")
    print(f"  🔧 OpenAPI spec:  http://{host}:{port}{app.openapi_url}")
    print("="*80)

    # Get all routes
    if HAS_NATIVE:
        routes = get_all_routes()
        print(f"\n🛣️  Registered {len(routes)} routes:")
        for route in routes:
            print(f"  {route['method']:6s} {route['path_pattern']}")

    return server
