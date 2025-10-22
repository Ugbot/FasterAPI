"""
FasterAPI - High-Performance Python Web Framework

A unified framework combining high-performance HTTP server and PostgreSQL driver.
Built with C++ hot paths for maximum performance.

Features:
- HTTP/1.1, HTTP/2, HTTP/3 support
- WebSocket support
- zstd compression
- PostgreSQL connection pooling
- FastAPI-compatible API
- Zero-copy operations
- Per-core sharding
"""

from typing import Any, Callable, Dict, List, Optional, Union
from .http import Server as HttpServer
from .http import Request, Response, WebSocket
from .http.sse import SSEConnection, SSEResponse
from .pg import PgPool, Pg, TxIsolation
from .pg.compat import Depends
from .core import Future, when_all, when_any, Reactor
from .core.combinators import (
    map_async, filter_async, reduce_async,
    retry_async, timeout_async, Pipeline
)


class App:
    """
    FasterAPI application with unified HTTP and PostgreSQL support.
    
    Features:
    - Multi-protocol HTTP server (H1/H2/H3)
    - WebSocket support
    - zstd compression
    - PostgreSQL integration
    - FastAPI-compatible decorators
    - Dependency injection
    - Middleware support
    """
    
    def __init__(
        self,
        port: int = 8000,
        host: str = "0.0.0.0",
        enable_h2: bool = False,
        enable_h3: bool = False,
        enable_compression: bool = True,
        **kwargs
    ):
        """
        Create a new FasterAPI application.
        
        Args:
            port: Server port
            host: Server host
            enable_h2: Enable HTTP/2 support
            enable_h3: Enable HTTP/3 support
            enable_compression: Enable zstd compression
            **kwargs: Additional configuration options
        """
        self.port = port
        self.host = host
        self.enable_h2 = enable_h2
        self.enable_h3 = enable_h3
        self.enable_compression = enable_compression
        
        # Create HTTP server
        self.server = HttpServer(
            port=port,
            host=host,
            enable_h2=enable_h2,
            enable_h3=enable_h3,
            enable_compression=enable_compression,
            **kwargs
        )
        
        # Route registry
        self.routes: Dict[str, List[Dict[str, Any]]] = {}
        
        # Middleware stack
        self.middleware: List[Callable] = []
        
        # Lifecycle hooks
        self.startup_hooks: List[Callable] = []
        self.shutdown_hooks: List[Callable] = []
        
        # Global state
        self.state: Dict[str, Any] = {}
    
    def get(self, path: str, **kwargs) -> Callable:
        """
        Add a GET route.
        
        Args:
            path: Route path
            **kwargs: Additional route options
            
        Returns:
            Decorator function
        """
        def decorator(func: Callable) -> Callable:
            self._add_route("GET", path, func, **kwargs)
            return func
        return decorator
    
    def post(self, path: str, **kwargs) -> Callable:
        """
        Add a POST route.
        
        Args:
            path: Route path
            **kwargs: Additional route options
            
        Returns:
            Decorator function
        """
        def decorator(func: Callable) -> Callable:
            self._add_route("POST", path, func, **kwargs)
            return func
        return decorator
    
    def put(self, path: str, **kwargs) -> Callable:
        """
        Add a PUT route.
        
        Args:
            path: Route path
            **kwargs: Additional route options
            
        Returns:
            Decorator function
        """
        def decorator(func: Callable) -> Callable:
            self._add_route("PUT", path, func, **kwargs)
            return func
        return decorator
    
    def delete(self, path: str, **kwargs) -> Callable:
        """
        Add a DELETE route.
        
        Args:
            path: Route path
            **kwargs: Additional route options
            
        Returns:
            Decorator function
        """
        def decorator(func: Callable) -> Callable:
            self._add_route("DELETE", path, func, **kwargs)
            return func
        return decorator
    
    def patch(self, path: str, **kwargs) -> Callable:
        """
        Add a PATCH route.
        
        Args:
            path: Route path
            **kwargs: Additional route options
            
        Returns:
            Decorator function
        """
        def decorator(func: Callable) -> Callable:
            self._add_route("PATCH", path, func, **kwargs)
            return func
        return decorator
    
    def websocket(self, path: str, **kwargs) -> Callable:
        """
        Add a WebSocket endpoint.
        
        Args:
            path: WebSocket path
            **kwargs: Additional WebSocket options
            
        Returns:
            Decorator function
            
        Example:
            @app.websocket("/ws")
            async def websocket_endpoint(ws: WebSocket):
                await ws.send_text("Connected!")
                while ws.is_open():
                    message = await ws.receive()
                    await ws.send_text(f"Echo: {message}")
        """
        def decorator(func: Callable) -> Callable:
            self._add_websocket(path, func, **kwargs)
            return func
        return decorator
    
    def sse(self, path: str, **kwargs) -> Callable:
        """
        Add a Server-Sent Events (SSE) endpoint.
        
        Args:
            path: SSE path
            **kwargs: Additional SSE options
            
        Returns:
            Decorator function
            
        Example:
            @app.sse("/events")
            def event_stream(sse: SSEConnection):
                for i in range(100):
                    sse.send(
                        {"count": i, "time": time.time()},
                        event="count",
                        id=str(i)
                    )
                    time.sleep(1)
        """
        def decorator(func: Callable) -> Callable:
            self._add_sse(path, func, **kwargs)
            return func
        return decorator
    
    def _add_route(self, method: str, path: str, handler: Callable, **kwargs) -> None:
        """Add a route to the server."""
        if method not in self.routes:
            self.routes[method] = []
        
        self.routes[method].append({
            'path': path,
            'handler': handler,
            'options': kwargs
        })
        
        # Create wrapper function that handles FastAPI-style dependencies
        def route_wrapper(req: Request, res: Response) -> None:
            try:
                # Apply middleware
                for middleware in self.middleware:
                    middleware(req, res)
                
                # Call the handler
                result = handler(req, res)
                
                # Handle different return types
                if isinstance(result, dict):
                    res.json(result).send()
                elif isinstance(result, str):
                    res.text(result).send()
                elif isinstance(result, Response):
                    result.send()
                else:
                    res.json({"result": str(result)}).send()
                    
            except Exception as e:
                res.status(500).json({"error": str(e)}).send()
        
        self.server.add_route(method, path, route_wrapper)
    
    def _add_websocket(self, path: str, handler: Callable, **kwargs) -> None:
        """Add a WebSocket endpoint to the server."""
        def websocket_wrapper(ws: WebSocket) -> None:
            try:
                handler(ws)
            except Exception as e:
                print(f"WebSocket error: {e}")
                ws.close(1011, "Internal error")
        
        self.server.add_websocket(path, websocket_wrapper)
    
    def _add_sse(self, path: str, handler: Callable, **kwargs) -> None:
        """Add an SSE endpoint to the server."""
        def sse_wrapper(req: Request, res: Response) -> None:
            try:
                # Set SSE headers
                res.set_header("Content-Type", "text/event-stream")
                res.set_header("Cache-Control", "no-cache")
                res.set_header("Connection", "keep-alive")
                res.set_header("X-Accel-Buffering", "no")
                
                # Create SSE connection
                # TODO: Get actual C++ handle from server
                from .http.sse import SSEConnection
                sse = SSEConnection(native_handle=None)  # Will be set by server
                
                # Call handler
                handler(sse)
                
            except Exception as e:
                print(f"SSE error: {e}")
                res.status(500).json({"error": str(e)}).send()
        
        # Register as GET route
        self._add_route("GET", path, sse_wrapper, **kwargs)
    
    def add_middleware(self, func: Callable) -> Callable:
        """
        Add middleware to the application.
        
        Args:
            func: Middleware function
            
        Returns:
            The middleware function
        """
        self.middleware.append(func)
        return func
    
    def on_event(self, event: str) -> Callable:
        """
        Add lifecycle event handler.
        
        Args:
            event: Event name ("startup" or "shutdown")
            
        Returns:
            Decorator function
        """
        def decorator(func: Callable) -> Callable:
            if event == "startup":
                self.startup_hooks.append(func)
            elif event == "shutdown":
                self.shutdown_hooks.append(func)
            return func
        return decorator
    
    def run(self, **kwargs) -> None:
        """
        Run the application.
        
        Args:
            **kwargs: Additional run options
        """
        # Run startup hooks
        for hook in self.startup_hooks:
            try:
                hook()
            except Exception as e:
                print(f"Startup hook error: {e}")
        
        # Start the server
        self.server.start()
        
        try:
            # Keep the application running
            import time
            while self.server.is_running():
                time.sleep(0.1)
        except KeyboardInterrupt:
            print("\n🛑 Shutting down...")
        finally:
            # Run shutdown hooks
            for hook in self.shutdown_hooks:
                try:
                    hook()
                except Exception as e:
                    print(f"Shutdown hook error: {e}")
            
            # Stop the server
            self.server.stop()
    
    def get_stats(self) -> Dict[str, Any]:
        """Get application statistics."""
        return {
            "server": self.server.get_stats(),
            "routes": {method: len(routes) for method, routes in self.routes.items()},
            "middleware_count": len(self.middleware),
            "startup_hooks": len(self.startup_hooks),
            "shutdown_hooks": len(self.shutdown_hooks),
        }


# Export main classes and functions
__all__ = [
    'App',
    'Depends',
    'PgPool',
    'Pg',
    'TxIsolation',
    'Request',
    'Response',
    'WebSocket',
    'SSEConnection',
    'SSEResponse',
    # Async utilities
    'Future',
    'when_all',
    'when_any',
    'Reactor',
    'map_async',
    'filter_async',
    'reduce_async',
    'retry_async',
    'timeout_async',
    'Pipeline',
]