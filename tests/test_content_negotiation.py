"""
Tests for content negotiation module.

Tests RFC 7231 compliant Accept header parsing and content negotiation.
"""

import random
import string

import pytest

from fasterapi.negotiation import (
    ContentNegotiator,
    MediaType,
    MediaTypes,
    accepts,
    get_accept_quality,
    parse_accept_header,
    select_media_type,
)


def random_string(length: int = 8) -> str:
    """Generate a random string."""
    return "".join(random.choices(string.ascii_lowercase, k=length))


class TestMediaTypeParsing:
    """Test MediaType parsing."""

    def test_parse_simple_type(self):
        """Test parsing simple media type."""
        mt = MediaType.parse("application/json")
        assert mt.type == "application"
        assert mt.subtype == "json"
        assert mt.q == 1.0
        assert mt.params == {}

    def test_parse_with_quality(self):
        """Test parsing media type with q-value."""
        mt = MediaType.parse("text/html; q=0.9")
        assert mt.type == "text"
        assert mt.subtype == "html"
        assert mt.q == 0.9

    def test_parse_with_charset(self):
        """Test parsing media type with charset parameter."""
        mt = MediaType.parse("text/html; charset=utf-8")
        assert mt.type == "text"
        assert mt.subtype == "html"
        assert mt.params["charset"] == "utf-8"
        assert mt.q == 1.0

    def test_parse_with_multiple_params(self):
        """Test parsing media type with multiple parameters."""
        mt = MediaType.parse("text/html; charset=utf-8; q=0.8")
        assert mt.type == "text"
        assert mt.subtype == "html"
        assert mt.params["charset"] == "utf-8"
        assert mt.q == 0.8

    def test_parse_wildcard_type(self):
        """Test parsing wildcard type."""
        mt = MediaType.parse("*/*")
        assert mt.type == "*"
        assert mt.subtype == "*"

    def test_parse_wildcard_subtype(self):
        """Test parsing wildcard subtype."""
        mt = MediaType.parse("text/*")
        assert mt.type == "text"
        assert mt.subtype == "*"

    def test_parse_case_insensitive(self):
        """Test that parsing is case insensitive."""
        mt = MediaType.parse("Application/JSON")
        assert mt.type == "application"
        assert mt.subtype == "json"

    def test_parse_clamps_q_value(self):
        """Test that q-value is clamped to [0, 1]."""
        mt = MediaType.parse("text/html; q=1.5")
        assert mt.q == 1.0

        mt = MediaType.parse("text/html; q=-0.5")
        assert mt.q == 0.0

    def test_parse_invalid_q_defaults_to_1(self):
        """Test that invalid q-value defaults to 1.0."""
        mt = MediaType.parse("text/html; q=invalid")
        assert mt.q == 1.0


class TestMediaTypeMatching:
    """Test MediaType matching logic."""

    def test_exact_match(self):
        """Test exact media type matching."""
        mt1 = MediaType.parse("application/json")
        mt2 = MediaType.parse("application/json")
        assert mt1.matches(mt2)
        assert mt2.matches(mt1)

    def test_wildcard_matches_all(self):
        """Test that */* matches everything."""
        wildcard = MediaType.parse("*/*")
        specific = MediaType.parse("application/json")
        assert wildcard.matches(specific)
        assert specific.matches(wildcard)

    def test_subtype_wildcard(self):
        """Test that type/* matches same type."""
        text_any = MediaType.parse("text/*")
        text_html = MediaType.parse("text/html")
        text_plain = MediaType.parse("text/plain")
        app_json = MediaType.parse("application/json")

        assert text_any.matches(text_html)
        assert text_any.matches(text_plain)
        assert not text_any.matches(app_json)

    def test_different_types_no_match(self):
        """Test that different types don't match."""
        mt1 = MediaType.parse("application/json")
        mt2 = MediaType.parse("text/html")
        assert not mt1.matches(mt2)


class TestMediaTypeSpecificity:
    """Test MediaType specificity calculation."""

    def test_wildcard_lowest_specificity(self):
        """Test that */* has lowest specificity."""
        wildcard = MediaType.parse("*/*")
        assert wildcard.specificity() == 0

    def test_type_wildcard_medium_specificity(self):
        """Test that type/* has medium specificity."""
        text_any = MediaType.parse("text/*")
        assert text_any.specificity() == 1

    def test_specific_type_high_specificity(self):
        """Test that type/subtype has high specificity."""
        specific = MediaType.parse("application/json")
        assert specific.specificity() == 2

    def test_params_increase_specificity(self):
        """Test that parameters increase specificity."""
        with_charset = MediaType.parse("text/html; charset=utf-8")
        without_charset = MediaType.parse("text/html")
        assert with_charset.specificity() > without_charset.specificity()


class TestParseAcceptHeader:
    """Test Accept header parsing."""

    def test_parse_single_type(self):
        """Test parsing single media type."""
        types = parse_accept_header("application/json")
        assert len(types) == 1
        assert types[0].type == "application"
        assert types[0].subtype == "json"

    def test_parse_multiple_types(self):
        """Test parsing multiple media types."""
        types = parse_accept_header("text/html, application/json")
        assert len(types) == 2

    def test_sorted_by_quality(self):
        """Test that results are sorted by q-value."""
        types = parse_accept_header("text/html; q=0.5, application/json; q=0.9")
        assert types[0].subtype == "json"  # Higher q first
        assert types[1].subtype == "html"

    def test_sorted_by_specificity_when_same_q(self):
        """Test sorting by specificity when q-values are equal."""
        types = parse_accept_header("*/*; q=0.5, text/*; q=0.5, text/html; q=0.5")
        # Most specific first when q is equal
        assert types[0].subtype == "html"
        assert types[1].subtype == "*"
        assert types[2].type == "*"

    def test_parse_empty_header(self):
        """Test parsing empty header defaults to */*."""
        types = parse_accept_header("")
        assert len(types) == 1
        assert types[0].type == "*"
        assert types[0].subtype == "*"

    def test_parse_complex_header(self):
        """Test parsing complex Accept header."""
        header = "text/html, application/xhtml+xml, application/xml;q=0.9, */*;q=0.8"
        types = parse_accept_header(header)
        assert len(types) == 4
        # First should be text/html or application/xhtml+xml (both q=1.0)
        assert types[0].q == 1.0
        # Last should be */* with q=0.8
        assert types[-1].type == "*"


class TestSelectMediaType:
    """Test media type selection."""

    def test_select_exact_match(self):
        """Test selecting exact match."""
        result = select_media_type(
            "application/json", ["application/json", "text/html"]
        )
        assert result == "application/json"

    def test_select_best_match(self):
        """Test selecting best match by q-value."""
        result = select_media_type(
            "text/html; q=0.5, application/json; q=0.9",
            ["application/json", "text/html"],
        )
        assert result == "application/json"

    def test_select_from_wildcard(self):
        """Test selecting from wildcard accept."""
        result = select_media_type("*/*", ["application/json", "text/html"])
        # Should return one of the available types
        assert result in ["application/json", "text/html"]

    def test_select_no_match(self):
        """Test no match returns None."""
        result = select_media_type("text/xml", ["application/json", "text/html"])
        assert result is None

    def test_select_with_default(self):
        """Test using default when no match."""
        result = select_media_type(
            "text/xml", ["application/json", "text/html"], default="application/json"
        )
        assert result == "application/json"

    def test_select_respects_q_zero(self):
        """Test that q=0 means not acceptable."""
        result = select_media_type(
            "application/json; q=0, text/html", ["application/json", "text/html"]
        )
        assert result == "text/html"

    def test_select_type_wildcard(self):
        """Test selecting with type/* wildcard."""
        result = select_media_type(
            "text/*", ["application/json", "text/html", "text/plain"]
        )
        assert result in ["text/html", "text/plain"]


class TestAcceptsFunction:
    """Test accepts() function."""

    def test_accepts_exact_match(self):
        """Test accepts with exact match."""
        assert accepts("application/json", "application/json")

    def test_accepts_wildcard(self):
        """Test accepts with wildcard."""
        assert accepts("*/*", "application/json")

    def test_accepts_type_wildcard(self):
        """Test accepts with type wildcard."""
        assert accepts("text/*", "text/html")
        assert not accepts("text/*", "application/json")

    def test_not_accepts(self):
        """Test not acceptable."""
        assert not accepts("text/html", "application/json")

    def test_not_accepts_q_zero(self):
        """Test q=0 is not acceptable."""
        assert not accepts("application/json; q=0", "application/json")


class TestGetAcceptQuality:
    """Test get_accept_quality() function."""

    def test_get_explicit_quality(self):
        """Test getting explicit q-value."""
        q = get_accept_quality("application/json; q=0.8", "application/json")
        assert q == 0.8

    def test_get_default_quality(self):
        """Test default q-value is 1.0."""
        q = get_accept_quality("application/json", "application/json")
        assert q == 1.0

    def test_get_wildcard_quality(self):
        """Test getting q-value from wildcard match."""
        q = get_accept_quality("*/*; q=0.5", "application/json")
        assert q == 0.5

    def test_get_no_match_quality(self):
        """Test q-value is 0 when no match."""
        q = get_accept_quality("text/html", "application/json")
        assert q == 0.0

    def test_specific_overrides_wildcard(self):
        """Test that specific match overrides wildcard."""
        q = get_accept_quality(
            "application/json; q=0.9, */*; q=0.5", "application/json"
        )
        assert q == 0.9


class TestContentNegotiator:
    """Test ContentNegotiator class."""

    def test_negotiate_json(self):
        """Test negotiating JSON."""
        neg = ContentNegotiator(["application/json", "text/html"])
        result = neg.negotiate("application/json")
        assert result == "application/json"

    def test_negotiate_with_default(self):
        """Test negotiating with default."""
        neg = ContentNegotiator(
            ["application/json", "text/html"], default_type="application/json"
        )
        result = neg.negotiate("text/xml")
        assert result == "application/json"

    def test_negotiate_auto_default(self):
        """Test auto default is first available."""
        neg = ContentNegotiator(["text/html", "application/json"])
        assert neg.default_type == "text/html"

    def test_accepts_method(self):
        """Test accepts method."""
        neg = ContentNegotiator(["application/json"])
        assert neg.accepts("*/*", "application/json")
        assert not neg.accepts("text/html", "application/json")

    def test_get_quality_method(self):
        """Test get_quality method."""
        neg = ContentNegotiator(["application/json"])
        q = neg.get_quality("application/json; q=0.7", "application/json")
        assert q == 0.7


class TestMediaTypesConstants:
    """Test MediaTypes constants."""

    def test_json_constant(self):
        """Test JSON constant."""
        assert MediaTypes.JSON == "application/json"

    def test_html_constant(self):
        """Test HTML constant."""
        assert MediaTypes.HTML == "text/html"

    def test_all_constants_are_valid(self):
        """Test all constants are valid media types."""
        constants = [
            MediaTypes.JSON,
            MediaTypes.HTML,
            MediaTypes.XML,
            MediaTypes.TEXT,
            MediaTypes.FORM,
            MediaTypes.MULTIPART,
            MediaTypes.OCTET_STREAM,
            MediaTypes.PDF,
            MediaTypes.PNG,
            MediaTypes.JPEG,
            MediaTypes.GIF,
            MediaTypes.SVG,
            MediaTypes.CSS,
            MediaTypes.JS,
            MediaTypes.WASM,
        ]
        for const in constants:
            mt = MediaType.parse(const)
            assert mt.type != ""
            assert mt.subtype != ""


class TestRandomizedNegotiation:
    """Test content negotiation with randomized inputs."""

    def test_random_type_selection(self):
        """Test selecting from random available types."""
        available = [
            f"application/{random_string()}",
            f"text/{random_string()}",
            f"image/{random_string()}",
        ]

        # Random type should match via wildcard
        neg = ContentNegotiator(available)
        result = neg.negotiate("*/*")
        assert result in available

    def test_random_quality_ordering(self):
        """Test that q-values are properly ordered."""
        types = []
        q_values = []

        for _ in range(5):
            q = round(random.random(), 2)
            type_str = f"application/{random_string()}; q={q}"
            types.append(type_str)
            q_values.append(q)

        header = ", ".join(types)
        parsed = parse_accept_header(header)

        # Verify sorted by q descending
        for i in range(len(parsed) - 1):
            assert parsed[i].q >= parsed[i + 1].q

    def test_many_types_negotiation(self):
        """Test negotiation with many types."""
        available = [f"type{i}/subtype{i}" for i in range(20)]

        # Create Accept header preferring a random type
        preferred_idx = random.randint(0, len(available) - 1)
        preferred = available[preferred_idx]

        neg = ContentNegotiator(available)
        result = neg.negotiate(preferred)
        assert result == preferred


class TestEdgeCases:
    """Test edge cases in content negotiation."""

    def test_empty_available_list(self):
        """Test with empty available list."""
        result = select_media_type("application/json", [])
        assert result is None

    def test_whitespace_in_header(self):
        """Test handling whitespace in Accept header."""
        result = select_media_type(
            "  application/json  ,  text/html  ", ["application/json", "text/html"]
        )
        assert result in ["application/json", "text/html"]

    def test_quoted_parameter_value(self):
        """Test parsing quoted parameter values."""
        mt = MediaType.parse('text/html; charset="utf-8"')
        assert mt.params["charset"] == "utf-8"

    def test_vendor_media_type(self):
        """Test vendor-specific media types."""
        mt = MediaType.parse("application/vnd.api+json")
        assert mt.type == "application"
        assert mt.subtype == "vnd.api+json"

    def test_suffix_media_type(self):
        """Test media type with suffix."""
        mt = MediaType.parse("application/hal+json")
        assert mt.subtype == "hal+json"

        # Should not match plain json
        json_mt = MediaType.parse("application/json")
        assert not mt.matches(json_mt)

    def test_str_representation(self):
        """Test string representation roundtrip."""
        original = "text/html; charset=utf-8; q=0.9"
        mt = MediaType.parse(original)
        result = str(mt)
        assert "text/html" in result
        assert "charset=utf-8" in result
        assert "q=0.9" in result


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
