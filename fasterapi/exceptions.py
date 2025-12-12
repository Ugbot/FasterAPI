"""
FastAPI-compatible exception classes for FasterAPI.

Provides HTTPException and validation error classes that match FastAPI's
exact interface and error response format.
"""

from typing import Any, Dict, List, Optional, Sequence, Union


class HTTPException(Exception):
    """
    HTTP exception that can be raised in handlers or dependencies.

    Matches FastAPI's HTTPException interface exactly.

    Usage:
        raise HTTPException(status_code=404, detail="Item not found")
        raise HTTPException(status_code=401, headers={"WWW-Authenticate": "Bearer"})

    Args:
        status_code: HTTP status code for the response
        detail: Any JSON-serializable data explaining the error
        headers: Optional dict of headers to include in the response
    """

    def __init__(
        self,
        status_code: int,
        detail: Any = None,
        headers: Optional[Dict[str, str]] = None,
    ) -> None:
        self.status_code = status_code
        self.detail = detail
        self.headers = headers
        super().__init__(detail)

    def __repr__(self) -> str:
        class_name = self.__class__.__name__
        return f"{class_name}(status_code={self.status_code!r}, detail={self.detail!r})"


class WebSocketException(Exception):
    """
    WebSocket exception for closing connections with a specific code.

    Usage:
        raise WebSocketException(code=1008, reason="Policy violation")

    Args:
        code: WebSocket close code (default: 1000 for normal closure)
        reason: Optional reason string
    """

    def __init__(
        self,
        code: int = 1000,
        reason: Optional[str] = None,
    ) -> None:
        self.code = code
        self.reason = reason or ""
        super().__init__(reason)

    def __repr__(self) -> str:
        return f"WebSocketException(code={self.code!r}, reason={self.reason!r})"


class ValidationError(Exception):
    """
    Base validation error class.

    Used internally for field validation errors.
    """

    def __init__(
        self,
        errors: List[Dict[str, Any]],
        body: Any = None,
    ) -> None:
        self._errors = errors
        self.body = body
        super().__init__(str(errors))

    def errors(self) -> List[Dict[str, Any]]:
        """Return list of validation errors."""
        return self._errors


class RequestValidationError(ValidationError):
    """
    Request validation error matching FastAPI's format.

    Raised when request data fails Pydantic validation.
    The error response format matches FastAPI exactly:

    {
        "detail": [
            {
                "type": "missing",
                "loc": ["body", "field_name"],
                "msg": "Field required",
                "input": {...}
            }
        ]
    }

    Args:
        errors: List of error dicts from Pydantic validation
        body: The request body that failed validation
    """

    def __init__(
        self,
        errors: Sequence[Any],
        *,
        body: Any = None,
    ) -> None:
        # Convert Pydantic errors to FastAPI format
        formatted_errors = []
        for error in errors:
            if isinstance(error, dict):
                formatted_errors.append(error)
            elif hasattr(error, "errors"):
                # Pydantic ValidationError
                for e in error.errors():
                    formatted_errors.append(self._format_pydantic_error(e))
            else:
                # Raw error, wrap it
                formatted_errors.append(
                    {
                        "type": "value_error",
                        "loc": ["body"],
                        "msg": str(error),
                        "input": body,
                    }
                )

        super().__init__(formatted_errors, body)

    @staticmethod
    def _format_pydantic_error(error: Dict[str, Any]) -> Dict[str, Any]:
        """Format a Pydantic error to match FastAPI's exact format."""
        return {
            "type": error.get("type", "value_error"),
            "loc": list(error.get("loc", [])),
            "msg": error.get("msg", "Validation error"),
            "input": error.get("input"),
        }


class ResponseValidationError(ValidationError):
    """
    Response validation error.

    Raised when response data fails to match the response_model.

    Args:
        errors: List of validation errors
        body: The response body that failed validation
    """

    pass


class StarletteHTTPException(HTTPException):
    """
    Alias for HTTPException for Starlette compatibility.

    Some code may import this from starlette.exceptions.
    """

    pass


class FastAPIError(RuntimeError):
    """
    Base error for FasterAPI framework errors.

    Raised for configuration or setup issues, not request handling.
    """

    pass


class RequestErrorModel:
    """
    Model for formatting validation error responses.

    Provides the structure for 422 Unprocessable Entity responses.
    """

    @staticmethod
    def format_errors(errors: List[Dict[str, Any]]) -> Dict[str, Any]:
        """
        Format errors into FastAPI's standard 422 response format.

        Args:
            errors: List of error dictionaries

        Returns:
            Dict matching FastAPI's validation error response
        """
        return {"detail": errors}


def format_validation_error_response(
    errors: Union[List[Dict[str, Any]], ValidationError],
) -> Dict[str, Any]:
    """
    Format validation errors into FastAPI's exact response structure.

    FastAPI returns 422 errors in this format:
    {
        "detail": [
            {
                "type": "missing",
                "loc": ["body", "name"],
                "msg": "Field required",
                "input": null
            }
        ]
    }

    Args:
        errors: Either a list of error dicts or a ValidationError instance

    Returns:
        Properly formatted error response dict
    """
    if isinstance(errors, ValidationError):
        error_list = errors.errors()
    else:
        error_list = errors

    return {"detail": error_list}


def convert_pydantic_validation_error(
    exc: Any,
    loc_prefix: tuple = ("body",),
) -> List[Dict[str, Any]]:
    """
    Convert a Pydantic ValidationError to FastAPI's error format.

    Args:
        exc: Pydantic ValidationError exception
        loc_prefix: Tuple to prepend to error locations (e.g., ("body",) or ("query",))

    Returns:
        List of error dicts in FastAPI format
    """
    errors = []

    if hasattr(exc, "errors"):
        for error in exc.errors():
            loc = list(loc_prefix) + list(error.get("loc", []))
            errors.append(
                {
                    "type": error.get("type", "value_error"),
                    "loc": loc,
                    "msg": error.get("msg", "Validation error"),
                    "input": error.get("input"),
                }
            )
    else:
        # Fallback for non-Pydantic errors
        errors.append(
            {
                "type": "value_error",
                "loc": list(loc_prefix),
                "msg": str(exc),
                "input": None,
            }
        )

    return errors
