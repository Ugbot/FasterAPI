"""
Content Negotiation Module

Implements RFC 7231 content negotiation for HTTP Accept headers.
Allows selecting the best response content type based on client preferences.

Features:
- Accept header parsing with q-value support
- Media type matching with wildcards
- Best match selection algorithm
- 406 Not Acceptable handling
"""

import re
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple, Union


@dataclass
class MediaType:
    """
    Represents a parsed media type with optional parameters.

    Examples:
        application/json
        text/html; charset=utf-8
        application/*
        */*
    """

    type: str  # e.g., "application", "text", "*"
    subtype: str  # e.g., "json", "html", "*"
    params: Dict[str, str]  # e.g., {"charset": "utf-8"}
    q: float = 1.0  # Quality value (0.0 - 1.0)

    @classmethod
    def parse(cls, media_type_str: str) -> "MediaType":
        """
        Parse a media type string into a MediaType object.

        Args:
            media_type_str: Media type string like "application/json; q=0.8"

        Returns:
            MediaType object
        """
        media_type_str = media_type_str.strip()
        params = {}
        q = 1.0

        # Split by semicolons to get parameters
        parts = [p.strip() for p in media_type_str.split(";")]
        type_part = parts[0]

        # Parse parameters
        for param in parts[1:]:
            if "=" in param:
                key, value = param.split("=", 1)
                key = key.strip().lower()
                value = value.strip().strip('"')
                if key == "q":
                    try:
                        q = float(value)
                        q = max(0.0, min(1.0, q))  # Clamp to [0, 1]
                    except ValueError:
                        q = 1.0
                else:
                    params[key] = value

        # Parse type/subtype
        if "/" in type_part:
            type_str, subtype_str = type_part.split("/", 1)
        else:
            type_str = type_part
            subtype_str = "*"

        return cls(
            type=type_str.strip().lower(),
            subtype=subtype_str.strip().lower(),
            params=params,
            q=q,
        )

    def matches(self, other: "MediaType") -> bool:
        """
        Check if this media type matches another (considering wildcards).

        Wildcards (*) match any value.

        Args:
            other: MediaType to compare against

        Returns:
            True if they match
        """
        # Check type match
        if self.type != "*" and other.type != "*":
            if self.type != other.type:
                return False

        # Check subtype match
        if self.subtype != "*" and other.subtype != "*":
            if self.subtype != other.subtype:
                return False

        return True

    def specificity(self) -> int:
        """
        Calculate specificity score for sorting.

        More specific types get higher scores:
        - */* = 0
        - type/* = 1
        - type/subtype = 2
        - type/subtype with params = 3+
        """
        score = 0
        if self.type != "*":
            score += 1
        if self.subtype != "*":
            score += 1
        score += len(self.params)
        return score

    def __str__(self) -> str:
        """Convert back to string representation."""
        result = f"{self.type}/{self.subtype}"
        for key, value in self.params.items():
            result += f"; {key}={value}"
        if self.q != 1.0:
            result += f"; q={self.q}"
        return result

    def __repr__(self) -> str:
        return f"MediaType({self!s})"


def parse_accept_header(accept_header: str) -> List[MediaType]:
    """
    Parse an HTTP Accept header into a list of MediaType objects.

    The result is sorted by:
    1. Quality value (q) descending
    2. Specificity descending

    Args:
        accept_header: Accept header value like "text/html, application/json; q=0.9"

    Returns:
        Sorted list of MediaType objects
    """
    if not accept_header:
        return [MediaType("*", "*", {}, 1.0)]

    media_types = []

    # Split by comma, handling potential commas in quoted strings
    # Simple approach: split and handle quotes
    parts = re.split(r',(?=(?:[^"]*"[^"]*")*[^"]*$)', accept_header)

    for part in parts:
        part = part.strip()
        if part:
            try:
                media_types.append(MediaType.parse(part))
            except Exception:
                # Skip invalid media types
                continue

    # Sort by q-value (descending) then specificity (descending)
    media_types.sort(key=lambda m: (-m.q, -m.specificity()))

    return media_types


def select_media_type(
    accept_header: str, available: List[str], default: Optional[str] = None
) -> Optional[str]:
    """
    Select the best media type based on Accept header and available types.

    Implements RFC 7231 content negotiation algorithm.

    Args:
        accept_header: Accept header value from request
        available: List of media types the server can produce
        default: Default media type if no match found (optional)

    Returns:
        Best matching media type string, or None if no acceptable match

    Examples:
        >>> select_media_type("application/json", ["application/json", "text/html"])
        "application/json"

        >>> select_media_type("text/html, application/json; q=0.9", ["application/json"])
        "application/json"

        >>> select_media_type("text/xml", ["application/json"])
        None
    """
    if not available:
        return default

    # Parse accept header
    accepted = parse_accept_header(accept_header)

    # Parse available types
    available_types = [MediaType.parse(t) for t in available]

    # Find best match
    best_match = None
    best_q = -1.0
    best_specificity = -1

    for accepted_type in accepted:
        if accepted_type.q == 0:
            continue  # q=0 means "not acceptable"

        for avail_type in available_types:
            if accepted_type.matches(avail_type):
                # Calculate match quality
                specificity = accepted_type.specificity()

                # Better match if higher q or same q with higher specificity
                if accepted_type.q > best_q or (
                    accepted_type.q == best_q and specificity > best_specificity
                ):
                    best_match = f"{avail_type.type}/{avail_type.subtype}"
                    best_q = accepted_type.q
                    best_specificity = specificity

    return best_match if best_match else default


def accepts(accept_header: str, media_type: str) -> bool:
    """
    Check if the Accept header accepts a specific media type.

    Args:
        accept_header: Accept header value from request
        media_type: Media type to check (e.g., "application/json")

    Returns:
        True if the media type is acceptable
    """
    accepted = parse_accept_header(accept_header)
    target = MediaType.parse(media_type)

    for accepted_type in accepted:
        if accepted_type.q > 0 and accepted_type.matches(target):
            return True

    return False


def get_accept_quality(accept_header: str, media_type: str) -> float:
    """
    Get the quality value for a specific media type from Accept header.

    Args:
        accept_header: Accept header value from request
        media_type: Media type to check

    Returns:
        Quality value (0.0 - 1.0), or 0.0 if not acceptable
    """
    accepted = parse_accept_header(accept_header)
    target = MediaType.parse(media_type)

    best_q = 0.0
    best_specificity = -1

    for accepted_type in accepted:
        if accepted_type.matches(target):
            specificity = accepted_type.specificity()
            # Higher specificity match takes precedence
            if specificity > best_specificity:
                best_q = accepted_type.q
                best_specificity = specificity
            elif specificity == best_specificity and accepted_type.q > best_q:
                best_q = accepted_type.q

    return best_q


class ContentNegotiator:
    """
    Content negotiation helper for request handling.

    Provides convenient methods for selecting response format based on
    client Accept header preferences.

    Example usage:
        negotiator = ContentNegotiator(["application/json", "text/html", "text/plain"])

        best_type = negotiator.negotiate(request.headers.get("Accept", "*/*"))
        if best_type is None:
            return Response(status_code=406)  # Not Acceptable
    """

    def __init__(self, available_types: List[str], default_type: Optional[str] = None):
        """
        Initialize content negotiator.

        Args:
            available_types: List of media types the server can produce
            default_type: Default type to return if no specific match
        """
        self.available_types = available_types
        self.default_type = default_type or (
            available_types[0] if available_types else None
        )

    def negotiate(self, accept_header: str) -> Optional[str]:
        """
        Select best media type for the given Accept header.

        Args:
            accept_header: Accept header value from request

        Returns:
            Best matching media type, or None if no acceptable match
        """
        return select_media_type(accept_header, self.available_types, self.default_type)

    def accepts(self, accept_header: str, media_type: str) -> bool:
        """
        Check if a specific media type is acceptable.

        Args:
            accept_header: Accept header value from request
            media_type: Media type to check

        Returns:
            True if acceptable
        """
        return accepts(accept_header, media_type)

    def get_quality(self, accept_header: str, media_type: str) -> float:
        """
        Get quality value for a specific media type.

        Args:
            accept_header: Accept header value from request
            media_type: Media type to check

        Returns:
            Quality value (0.0 - 1.0)
        """
        return get_accept_quality(accept_header, media_type)


# Common media type constants
class MediaTypes:
    """Common media type constants."""

    JSON = "application/json"
    HTML = "text/html"
    XML = "application/xml"
    TEXT = "text/plain"
    FORM = "application/x-www-form-urlencoded"
    MULTIPART = "multipart/form-data"
    OCTET_STREAM = "application/octet-stream"
    PDF = "application/pdf"
    PNG = "image/png"
    JPEG = "image/jpeg"
    GIF = "image/gif"
    SVG = "image/svg+xml"
    CSS = "text/css"
    JS = "application/javascript"
    WASM = "application/wasm"
