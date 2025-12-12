"""
FastAPI-compatible APIRouter for FasterAPI.

Provides APIRouter for organizing routes into groups with shared
prefixes, tags, and dependencies.

Usage:
    from fasterapi import APIRouter

    router = APIRouter(prefix="/users", tags=["users"])

    @router.get("/")
    def list_users():
        return []

    @router.get("/{user_id}")
    def get_user(user_id: int):
        return {"id": user_id}

    # In main app
    app.include_router(router)
"""

from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Sequence, Set, Type, Union

try:
    from pydantic import BaseModel

    HAS_PYDANTIC = True
except ImportError:
    HAS_PYDANTIC = False
    BaseModel = object

from fasterapi.params import Depends


@dataclass
class RouteDefinition:
    """
    Stores route metadata before registration with the main app.

    Routes are collected here and then registered when the router
    is included in an application.
    """

    method: str
    path: str
    handler: Callable
    response_model: Optional[Type] = None
    status_code: Optional[int] = None
    summary: str = ""
    description: str = ""
    tags: List[str] = field(default_factory=list)
    dependencies: List[Depends] = field(default_factory=list)
    deprecated: bool = False
    responses: Dict[int, Dict[str, Any]] = field(default_factory=dict)
    operation_id: Optional[str] = None
    include_in_schema: bool = True
    name: Optional[str] = None


class APIRouter:
    """
    FastAPI-compatible router for organizing routes into groups.

    Allows grouping routes with shared prefix, tags, and dependencies.
    Supports nesting routers via include_router().

    Usage:
        router = APIRouter(
            prefix="/api/v1",
            tags=["v1"],
            dependencies=[Depends(verify_api_key)],
        )

        @router.get("/items")
        def list_items():
            return []

        app.include_router(router)
    """

    def __init__(
        self,
        *,
        prefix: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        default_response_class: Optional[Type] = None,
        redirect_slashes: bool = True,
        deprecated: bool = False,
        include_in_schema: bool = True,
    ):
        """
        Initialize APIRouter.

        Args:
            prefix: URL prefix for all routes in this router
            tags: Default tags for all routes
            dependencies: Dependencies applied to all routes
            responses: Default responses for OpenAPI
            default_response_class: Default response class
            redirect_slashes: Auto-redirect trailing slashes
            deprecated: Mark all routes as deprecated
            include_in_schema: Include routes in OpenAPI schema
        """
        # Normalize prefix
        self.prefix = prefix.rstrip("/") if prefix else ""
        if self.prefix and not self.prefix.startswith("/"):
            self.prefix = "/" + self.prefix

        self.tags = list(tags) if tags else []
        self.dependencies = list(dependencies) if dependencies else []
        self.responses = dict(responses) if responses else {}
        self.default_response_class = default_response_class
        self.redirect_slashes = redirect_slashes
        self.deprecated = deprecated
        self.include_in_schema = include_in_schema

        # Storage for routes and sub-routers
        self._routes: List[RouteDefinition] = []
        self._sub_routers: List[tuple] = []  # (router, prefix, tags, deps)

        # Lifecycle hooks
        self._on_startup: List[Callable] = []
        self._on_shutdown: List[Callable] = []

    def _add_route(
        self,
        method: str,
        path: str,
        handler: Callable,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> None:
        """Add a route to this router."""
        # Normalize path
        if path and not path.startswith("/"):
            path = "/" + path

        # Merge tags
        route_tags = list(self.tags)
        if tags:
            route_tags.extend(tags)

        # Merge dependencies
        route_deps = list(self.dependencies)
        if dependencies:
            route_deps.extend(dependencies)

        # Merge responses
        route_responses = dict(self.responses)
        if responses:
            route_responses.update(responses)

        # Determine deprecation
        if deprecated is None:
            deprecated = self.deprecated

        # Determine schema inclusion
        if include_in_schema is None:
            include_in_schema = self.include_in_schema

        route = RouteDefinition(
            method=method.upper(),
            path=path,
            handler=handler,
            response_model=response_model,
            status_code=status_code,
            summary=summary,
            description=description,
            tags=route_tags,
            dependencies=route_deps,
            deprecated=deprecated,
            responses=route_responses,
            operation_id=operation_id,
            include_in_schema=include_in_schema,
            name=name,
        )

        self._routes.append(route)

    def get(
        self,
        path: str,
        *,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> Callable[[Callable], Callable]:
        """GET route decorator."""

        def decorator(func: Callable) -> Callable:
            self._add_route(
                "GET",
                path,
                func,
                response_model,
                status_code,
                summary,
                description,
                tags,
                dependencies,
                deprecated,
                responses,
                operation_id,
                include_in_schema,
                name,
                **kwargs,
            )
            return func

        return decorator

    def post(
        self,
        path: str,
        *,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> Callable[[Callable], Callable]:
        """POST route decorator."""

        def decorator(func: Callable) -> Callable:
            self._add_route(
                "POST",
                path,
                func,
                response_model,
                status_code,
                summary,
                description,
                tags,
                dependencies,
                deprecated,
                responses,
                operation_id,
                include_in_schema,
                name,
                **kwargs,
            )
            return func

        return decorator

    def put(
        self,
        path: str,
        *,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> Callable[[Callable], Callable]:
        """PUT route decorator."""

        def decorator(func: Callable) -> Callable:
            self._add_route(
                "PUT",
                path,
                func,
                response_model,
                status_code,
                summary,
                description,
                tags,
                dependencies,
                deprecated,
                responses,
                operation_id,
                include_in_schema,
                name,
                **kwargs,
            )
            return func

        return decorator

    def delete(
        self,
        path: str,
        *,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> Callable[[Callable], Callable]:
        """DELETE route decorator."""

        def decorator(func: Callable) -> Callable:
            self._add_route(
                "DELETE",
                path,
                func,
                response_model,
                status_code,
                summary,
                description,
                tags,
                dependencies,
                deprecated,
                responses,
                operation_id,
                include_in_schema,
                name,
                **kwargs,
            )
            return func

        return decorator

    def patch(
        self,
        path: str,
        *,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> Callable[[Callable], Callable]:
        """PATCH route decorator."""

        def decorator(func: Callable) -> Callable:
            self._add_route(
                "PATCH",
                path,
                func,
                response_model,
                status_code,
                summary,
                description,
                tags,
                dependencies,
                deprecated,
                responses,
                operation_id,
                include_in_schema,
                name,
                **kwargs,
            )
            return func

        return decorator

    def options(
        self,
        path: str,
        *,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> Callable[[Callable], Callable]:
        """OPTIONS route decorator."""

        def decorator(func: Callable) -> Callable:
            self._add_route(
                "OPTIONS",
                path,
                func,
                response_model,
                status_code,
                summary,
                description,
                tags,
                dependencies,
                deprecated,
                responses,
                operation_id,
                include_in_schema,
                name,
                **kwargs,
            )
            return func

        return decorator

    def head(
        self,
        path: str,
        *,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> Callable[[Callable], Callable]:
        """HEAD route decorator."""

        def decorator(func: Callable) -> Callable:
            self._add_route(
                "HEAD",
                path,
                func,
                response_model,
                status_code,
                summary,
                description,
                tags,
                dependencies,
                deprecated,
                responses,
                operation_id,
                include_in_schema,
                name,
                **kwargs,
            )
            return func

        return decorator

    def add_api_route(
        self,
        path: str,
        endpoint: Callable,
        *,
        methods: Optional[List[str]] = None,
        response_model: Optional[Type] = None,
        status_code: Optional[int] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        deprecated: Optional[bool] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
        operation_id: Optional[str] = None,
        include_in_schema: Optional[bool] = None,
        name: Optional[str] = None,
        **kwargs,
    ) -> None:
        """
        Add a route programmatically.

        Args:
            path: URL path
            endpoint: Handler function
            methods: List of HTTP methods (default: ["GET"])
            **kwargs: Additional route options
        """
        methods = methods or ["GET"]
        for method in methods:
            self._add_route(
                method,
                path,
                endpoint,
                response_model,
                status_code,
                summary,
                description,
                tags,
                dependencies,
                deprecated,
                responses,
                operation_id,
                include_in_schema,
                name,
                **kwargs,
            )

    def include_router(
        self,
        router: "APIRouter",
        *,
        prefix: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
    ) -> None:
        """
        Include another router's routes.

        This allows nesting routers for modular route organization.

        Args:
            router: The router to include
            prefix: Additional prefix to prepend
            tags: Additional tags to add
            dependencies: Additional dependencies to apply
            responses: Additional responses to merge
        """
        self._sub_routers.append(
            (router, prefix, tags or [], dependencies or [], responses or {})
        )

    def on_event(self, event_type: str) -> Callable[[Callable], Callable]:
        """
        Register a lifecycle event handler.

        Args:
            event_type: Either "startup" or "shutdown"

        Returns:
            Decorator function
        """

        def decorator(func: Callable) -> Callable:
            if event_type == "startup":
                self._on_startup.append(func)
            elif event_type == "shutdown":
                self._on_shutdown.append(func)
            else:
                raise ValueError(f"Invalid event type: {event_type}")
            return func

        return decorator

    def get_routes(
        self,
        prefix: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[Sequence[Depends]] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
    ) -> List[RouteDefinition]:
        """
        Get all routes with merged prefix, tags, and dependencies.

        This is called by the main app when including the router.

        Args:
            prefix: Additional prefix from include_router
            tags: Additional tags from include_router
            dependencies: Additional dependencies from include_router
            responses: Additional responses from include_router

        Returns:
            List of RouteDefinition with merged settings
        """
        # Calculate final prefix
        final_prefix = prefix.rstrip("/") + self.prefix

        # Merge tags
        merged_tags = list(tags or [])

        # Merge dependencies
        merged_deps = list(dependencies or [])

        # Merge responses
        merged_responses = dict(responses or {})

        routes = []

        # Process direct routes
        for route in self._routes:
            # Build final path
            final_path = final_prefix + route.path

            # Merge route-specific settings
            final_tags = merged_tags + route.tags
            final_deps = merged_deps + route.dependencies
            final_responses = {**merged_responses, **route.responses}

            routes.append(
                RouteDefinition(
                    method=route.method,
                    path=final_path,
                    handler=route.handler,
                    response_model=route.response_model,
                    status_code=route.status_code,
                    summary=route.summary,
                    description=route.description,
                    tags=final_tags,
                    dependencies=final_deps,
                    deprecated=route.deprecated,
                    responses=final_responses,
                    operation_id=route.operation_id,
                    include_in_schema=route.include_in_schema,
                    name=route.name,
                )
            )

        # Process sub-routers recursively
        for (
            sub_router,
            sub_prefix,
            sub_tags,
            sub_deps,
            sub_responses,
        ) in self._sub_routers:
            # Calculate prefix for sub-router
            sub_final_prefix = final_prefix + (
                sub_prefix.rstrip("/") if sub_prefix else ""
            )

            # Merge settings
            sub_final_tags = merged_tags + list(sub_tags)
            sub_final_deps = merged_deps + list(sub_deps)
            sub_final_responses = {**merged_responses, **sub_responses}

            # Get routes from sub-router
            sub_routes = sub_router.get_routes(
                prefix=sub_final_prefix,
                tags=sub_final_tags,
                dependencies=sub_final_deps,
                responses=sub_final_responses,
            )
            routes.extend(sub_routes)

        return routes

    @property
    def routes(self) -> List[RouteDefinition]:
        """Get all routes without additional merging."""
        return self.get_routes()
