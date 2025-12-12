"""
FastAPI-compatible parameter descriptors for FasterAPI.

Provides Query, Path, Header, Cookie, Body, Form, and File parameter markers
that match FastAPI's interface exactly.

Usage:
    from fasterapi import Query, Path, Header, Cookie, Body, Form, File

    @app.get("/items/{item_id}")
    def get_item(
        item_id: int = Path(..., description="The item ID"),
        q: str = Query(None, min_length=3, max_length=50),
        x_token: str = Header(..., alias="X-Token"),
    ):
        return {"item_id": item_id, "q": q}
"""

import re
from typing import Any, Callable, Dict, List, Optional, Pattern, Sequence, Type, Union


# Sentinel value for required parameters (matches FastAPI's ...)
class _Required:
    """Sentinel class to indicate a required parameter."""

    def __repr__(self) -> str:
        return "..."


# Export as Ellipsis-like object for `= Query(...)` syntax
Required = _Required()


class ParamBase:
    """
    Base class for all parameter descriptors.

    All parameter types (Query, Path, Header, Cookie, Body, Form, File)
    inherit from this class and share common validation logic.
    """

    __slots__ = (
        "default",
        "default_factory",
        "alias",
        "title",
        "description",
        "gt",
        "ge",
        "lt",
        "le",
        "min_length",
        "max_length",
        "regex",
        "deprecated",
        "include_in_schema",
        "examples",
        "example",
        "_location",
    )

    def __init__(
        self,
        default: Any = ...,
        *,
        default_factory: Optional[Callable[[], Any]] = None,
        alias: Optional[str] = None,
        title: Optional[str] = None,
        description: Optional[str] = None,
        gt: Optional[float] = None,
        ge: Optional[float] = None,
        lt: Optional[float] = None,
        le: Optional[float] = None,
        min_length: Optional[int] = None,
        max_length: Optional[int] = None,
        regex: Optional[str] = None,
        deprecated: bool = False,
        include_in_schema: bool = True,
        examples: Optional[List[Any]] = None,
        example: Any = None,
    ):
        """
        Initialize parameter descriptor.

        Args:
            default: Default value (use ... for required)
            default_factory: Factory function for default value
            alias: Alternative name for the parameter
            title: Title for OpenAPI schema
            description: Description for OpenAPI schema
            gt: Value must be greater than this
            ge: Value must be greater than or equal to this
            lt: Value must be less than this
            le: Value must be less than or equal to this
            min_length: Minimum length for strings
            max_length: Maximum length for strings
            regex: Regex pattern for string validation
            deprecated: Mark as deprecated in OpenAPI
            include_in_schema: Include in OpenAPI schema
            examples: List of example values
            example: Single example value
        """
        self.default = default
        self.default_factory = default_factory
        self.alias = alias
        self.title = title
        self.description = description
        self.gt = gt
        self.ge = ge
        self.lt = lt
        self.le = le
        self.min_length = min_length
        self.max_length = max_length
        self.regex = regex
        self.deprecated = deprecated
        self.include_in_schema = include_in_schema
        self.examples = examples
        self.example = example
        self._location = "query"  # Default, overridden in subclasses

    @property
    def required(self) -> bool:
        """Check if this parameter is required."""
        return self.default is ... and self.default_factory is None

    def get_default(self) -> Any:
        """Get the default value, calling factory if needed."""
        if self.default_factory is not None:
            return self.default_factory()
        return self.default if self.default is not ... else None

    def validate(self, value: Any, param_name: str) -> Any:
        """
        Validate a value against this parameter's constraints.

        Args:
            value: The value to validate
            param_name: Parameter name (for error messages)

        Returns:
            The validated value

        Raises:
            ValueError: If validation fails
        """
        if value is None:
            if self.required:
                raise ValueError(f"Field required")
            return self.get_default()

        # Numeric constraints
        if self.gt is not None and value <= self.gt:
            raise ValueError(f"Input should be greater than {self.gt}")
        if self.ge is not None and value < self.ge:
            raise ValueError(f"Input should be greater than or equal to {self.ge}")
        if self.lt is not None and value >= self.lt:
            raise ValueError(f"Input should be less than {self.lt}")
        if self.le is not None and value > self.le:
            raise ValueError(f"Input should be less than or equal to {self.le}")

        # String constraints
        if isinstance(value, str):
            if self.min_length is not None and len(value) < self.min_length:
                raise ValueError(
                    f"String should have at least {self.min_length} characters"
                )
            if self.max_length is not None and len(value) > self.max_length:
                raise ValueError(
                    f"String should have at most {self.max_length} characters"
                )
            if self.regex is not None:
                if not re.match(self.regex, value):
                    raise ValueError(f"String should match pattern '{self.regex}'")

        return value

    def to_openapi_param(self, name: str, param_type: str) -> Dict[str, Any]:
        """
        Convert to OpenAPI parameter schema.

        Args:
            name: Parameter name
            param_type: OpenAPI type string

        Returns:
            OpenAPI parameter dict
        """
        schema: Dict[str, Any] = {"type": param_type}

        if self.gt is not None:
            schema["exclusiveMinimum"] = self.gt
        if self.ge is not None:
            schema["minimum"] = self.ge
        if self.lt is not None:
            schema["exclusiveMaximum"] = self.lt
        if self.le is not None:
            schema["maximum"] = self.le
        if self.min_length is not None:
            schema["minLength"] = self.min_length
        if self.max_length is not None:
            schema["maxLength"] = self.max_length
        if self.regex is not None:
            schema["pattern"] = self.regex

        param: Dict[str, Any] = {
            "name": self.alias or name,
            "in": self._location,
            "required": self.required,
            "schema": schema,
        }

        if self.description:
            param["description"] = self.description
        if self.deprecated:
            param["deprecated"] = True
        if self.example is not None:
            param["example"] = self.example
        if self.examples:
            param["examples"] = self.examples

        return param

    def __repr__(self) -> str:
        class_name = self.__class__.__name__
        if self.required:
            return f"{class_name}(...)"
        return f"{class_name}(default={self.default!r})"


class Query(ParamBase):
    """
    Query parameter descriptor.

    Usage:
        @app.get("/items")
        def list_items(
            skip: int = Query(0, ge=0),
            limit: int = Query(10, ge=1, le=100),
            q: str = Query(None, min_length=3),
        ):
            return items[skip:skip+limit]
    """

    def __init__(self, default: Any = ..., **kwargs):
        super().__init__(default, **kwargs)
        self._location = "query"


class Path(ParamBase):
    """
    Path parameter descriptor.

    Path parameters are always required.

    Usage:
        @app.get("/items/{item_id}")
        def get_item(
            item_id: int = Path(..., description="The item ID", gt=0),
        ):
            return items[item_id]
    """

    def __init__(self, default: Any = ..., **kwargs):
        # Path parameters are always required
        super().__init__(default, **kwargs)
        self._location = "path"

    @property
    def required(self) -> bool:
        """Path parameters are always required."""
        return True


class Header(ParamBase):
    """
    Header parameter descriptor.

    By default, header names are converted from Python snake_case to
    HTTP header format (X-Header-Name). Use `alias` to override.

    Usage:
        @app.get("/items")
        def list_items(
            x_token: str = Header(...),  # Looks for X-Token header
            user_agent: str = Header(None, alias="User-Agent"),
        ):
            return {"token": x_token}
    """

    def __init__(
        self,
        default: Any = ...,
        *,
        convert_underscores: bool = True,
        **kwargs,
    ):
        super().__init__(default, **kwargs)
        self._location = "header"
        self.convert_underscores = convert_underscores

    def get_header_name(self, param_name: str) -> str:
        """
        Get the HTTP header name for this parameter.

        Args:
            param_name: Python parameter name

        Returns:
            HTTP header name
        """
        if self.alias:
            return self.alias

        if self.convert_underscores:
            # Convert snake_case to Header-Case
            # x_token -> X-Token
            parts = param_name.split("_")
            return "-".join(part.capitalize() for part in parts)

        return param_name


class Cookie(ParamBase):
    """
    Cookie parameter descriptor.

    Usage:
        @app.get("/items")
        def list_items(
            session_id: str = Cookie(None),
            preferences: str = Cookie(None, alias="user_prefs"),
        ):
            return {"session": session_id}
    """

    def __init__(self, default: Any = ..., **kwargs):
        super().__init__(default, **kwargs)
        self._location = "cookie"


class Body(ParamBase):
    """
    Body parameter descriptor.

    Used to explicitly mark a parameter as coming from the request body,
    or to provide additional validation/documentation for body parameters.

    Usage:
        @app.post("/items")
        def create_item(
            item: Item = Body(..., embed=True),
            importance: int = Body(1, ge=1, le=5),
        ):
            return {"item": item, "importance": importance}
    """

    def __init__(
        self,
        default: Any = ...,
        *,
        embed: bool = False,
        media_type: str = "application/json",
        **kwargs,
    ):
        super().__init__(default, **kwargs)
        self._location = "body"
        self.embed = embed
        self.media_type = media_type


class Form(ParamBase):
    """
    Form field parameter descriptor.

    Used for form data (application/x-www-form-urlencoded or multipart/form-data).

    Usage:
        @app.post("/login")
        def login(
            username: str = Form(...),
            password: str = Form(...),
        ):
            return {"username": username}
    """

    def __init__(
        self,
        default: Any = ...,
        *,
        media_type: str = "application/x-www-form-urlencoded",
        **kwargs,
    ):
        super().__init__(default, **kwargs)
        self._location = "body"
        self.media_type = media_type


class File(ParamBase):
    """
    File upload parameter descriptor.

    Used with UploadFile type for file uploads.

    Usage:
        @app.post("/upload")
        async def upload_file(
            file: UploadFile = File(..., description="File to upload"),
        ):
            return {"filename": file.filename}
    """

    def __init__(
        self,
        default: Any = ...,
        *,
        media_type: str = "multipart/form-data",
        **kwargs,
    ):
        super().__init__(default, **kwargs)
        self._location = "body"
        self.media_type = media_type


class Depends:
    """
    Dependency injection marker.

    Marks a parameter as a dependency that should be resolved by calling
    the provided callable.

    Usage:
        def get_db():
            db = Database()
            try:
                yield db
            finally:
                db.close()

        @app.get("/items")
        def list_items(db = Depends(get_db)):
            return db.query("SELECT * FROM items")
    """

    __slots__ = ("dependency", "use_cache")

    def __init__(
        self,
        dependency: Optional[Callable[..., Any]] = None,
        *,
        use_cache: bool = True,
    ):
        """
        Create a dependency marker.

        Args:
            dependency: Callable to invoke for the dependency value.
                       If None, the type annotation is used.
            use_cache: If True, cache the result within the request scope.
        """
        self.dependency = dependency
        self.use_cache = use_cache

    def __repr__(self) -> str:
        dep_name = self.dependency.__name__ if self.dependency else "None"
        return f"Depends({dep_name})"


class Security(Depends):
    """
    Security dependency marker.

    Similar to Depends, but used specifically for security schemes.
    Includes additional scopes parameter for OAuth2.

    Usage:
        oauth2_scheme = OAuth2PasswordBearer(tokenUrl="token")

        @app.get("/users/me")
        def get_current_user(token: str = Security(oauth2_scheme, scopes=["read"])):
            return decode_token(token)
    """

    __slots__ = ("dependency", "use_cache", "scopes")

    def __init__(
        self,
        dependency: Optional[Callable[..., Any]] = None,
        *,
        scopes: Optional[Sequence[str]] = None,
        use_cache: bool = True,
    ):
        """
        Create a security dependency marker.

        Args:
            dependency: Security scheme callable
            scopes: Required OAuth2 scopes
            use_cache: If True, cache the result within the request scope
        """
        super().__init__(dependency, use_cache=use_cache)
        self.scopes = list(scopes) if scopes else []

    def __repr__(self) -> str:
        dep_name = self.dependency.__name__ if self.dependency else "None"
        return f"Security({dep_name}, scopes={self.scopes})"


def is_param_descriptor(value: Any) -> bool:
    """Check if a value is a parameter descriptor."""
    return isinstance(value, (ParamBase, Depends, Security))


def get_param_location(descriptor: Any) -> str:
    """Get the parameter location from a descriptor."""
    if isinstance(descriptor, ParamBase):
        return descriptor._location
    return "depends"
