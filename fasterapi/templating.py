"""
Template rendering for FasterAPI.

Compatible with Starlette's Jinja2Templates interface.

Usage:
    from fasterapi.templating import Jinja2Templates

    templates = Jinja2Templates(directory="templates")

    @app.get("/", response_class=HTMLResponse)
    async def homepage(request: Request):
        return templates.TemplateResponse("index.html", {"request": request})
"""

import typing
from typing import Any, Callable, Dict, Mapping, Optional, Union

try:
    from jinja2 import Environment, FileSystemLoader, select_autoescape

    JINJA2_AVAILABLE = True
except ImportError:
    JINJA2_AVAILABLE = False
    Environment = None
    FileSystemLoader = None


class _TemplateResponse:
    """
    Template response that renders a Jinja2 template.
    """

    media_type = "text/html"
    charset = "utf-8"

    def __init__(
        self,
        template: Any,
        context: Dict[str, Any],
        status_code: int = 200,
        headers: Optional[Mapping[str, str]] = None,
        media_type: Optional[str] = None,
        background: Optional[Any] = None,
    ):
        self.template = template
        self.context = context
        self.status_code = status_code
        self.headers = dict(headers) if headers else {}
        self.background = background

        if media_type is not None:
            self.media_type = media_type

        # Render template
        self.body = self._render()

        # Set content-type header
        if "content-type" not in {k.lower() for k in self.headers}:
            content_type = f"{self.media_type}; charset={self.charset}"
            self.headers["Content-Type"] = content_type

        # Set content-length
        if "content-length" not in {k.lower() for k in self.headers}:
            self.headers["Content-Length"] = str(len(self.body))

    def _render(self) -> bytes:
        """Render the template to bytes."""
        content = self.template.render(self.context)
        return content.encode(self.charset)


class Jinja2Templates:
    """
    Jinja2 template renderer.

    Usage:
        templates = Jinja2Templates(directory="templates")

        @app.get("/")
        async def homepage(request: Request):
            return templates.TemplateResponse(
                "index.html",
                {"request": request, "title": "Home"}
            )
    """

    def __init__(
        self,
        directory: Union[str, typing.List[str]] = None,
        context_processors: typing.List[Callable] = None,
        **env_options: Any,
    ):
        """
        Initialize Jinja2 templates.

        Args:
            directory: Template directory or list of directories
            context_processors: List of context processor functions
            **env_options: Additional options passed to jinja2.Environment
        """
        if not JINJA2_AVAILABLE:
            raise ImportError(
                "jinja2 is required for templating. Install it with: pip install jinja2"
            )

        # Handle directory as string or list
        if isinstance(directory, str):
            directories = [directory]
        else:
            directories = list(directory) if directory else ["."]

        # Set up default options
        default_options = {
            "autoescape": select_autoescape(["html", "xml"]),
            "enable_async": True,
        }
        default_options.update(env_options)

        # Create Jinja2 environment
        self.env = Environment(
            loader=FileSystemLoader(directories),
            **default_options,
        )

        self.context_processors = context_processors or []

    def get_template(self, name: str) -> Any:
        """Get a template by name."""
        return self.env.get_template(name)

    def TemplateResponse(
        self,
        name: str,
        context: Dict[str, Any],
        status_code: int = 200,
        headers: Optional[Mapping[str, str]] = None,
        media_type: Optional[str] = None,
        background: Optional[Any] = None,
    ) -> _TemplateResponse:
        """
        Create a template response.

        Args:
            name: Template name (relative to template directory)
            context: Template context dict (must include "request")
            status_code: HTTP status code
            headers: Additional response headers
            media_type: Response media type
            background: Background task

        Returns:
            TemplateResponse instance
        """
        # Apply context processors
        for processor in self.context_processors:
            context.update(processor(context.get("request")))

        # Get and render template
        template = self.get_template(name)

        return _TemplateResponse(
            template=template,
            context=context,
            status_code=status_code,
            headers=headers,
            media_type=media_type,
            background=background,
        )


# Alias for compatibility
TemplateResponse = _TemplateResponse


__all__ = ["Jinja2Templates", "TemplateResponse"]
