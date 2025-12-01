#!/usr/bin/env python3
"""
Parameter Extraction Edge Case Tests

Comprehensive tests for edge cases in path parameter extraction,
query parameter parsing, and URL decoding. Tests the C++ parameter
extractor bindings directly without requiring a running server.
"""

import sys
import random
import string
import pytest

sys.path.insert(0, '/Users/bengamble/FasterAPI')

# Try to import the native module
try:
    from fasterapi import _fastapi_native
    NATIVE_AVAILABLE = True
except ImportError:
    NATIVE_AVAILABLE = False


# =============================================================================
# Test Helpers
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate a random alphanumeric string."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


def random_int(min_val: int = 1, max_val: int = 1000) -> int:
    """Generate a random integer."""
    return random.randint(min_val, max_val)


def generate_url_encoded_string(s: str) -> str:
    """Generate URL-encoded version of a string."""
    result = []
    for c in s:
        if c.isalnum() or c in '-_.~':
            result.append(c)
        elif c == ' ':
            result.append('%20')
        else:
            result.append('%{:02X}'.format(ord(c)))
    return ''.join(result)


# =============================================================================
# Skip if native module not available
# =============================================================================

pytestmark = pytest.mark.skipif(
    not NATIVE_AVAILABLE,
    reason="Native module not available"
)


# =============================================================================
# Path Parameter Extraction Tests - Edge Cases
# =============================================================================

class TestPathParamExtraction:
    """Edge case tests for path parameter extraction."""

    def test_single_parameter_at_root(self):
        """Test single parameter directly at root."""
        result = _fastapi_native.extract_path_params("/{id}")
        assert result == ["id"]

    def test_consecutive_parameters(self):
        """Test multiple consecutive parameters."""
        result = _fastapi_native.extract_path_params("/items/{id1}/{id2}/{id3}")
        assert result == ["id1", "id2", "id3"]

    def test_parameter_with_underscores(self):
        """Test parameter names with underscores."""
        result = _fastapi_native.extract_path_params("/users/{user_account_id}")
        assert result == ["user_account_id"]

    def test_parameter_with_numbers(self):
        """Test parameter names with numbers."""
        result = _fastapi_native.extract_path_params("/v2/items/{item1_id}")
        assert result == ["item1_id"]

    def test_empty_pattern(self):
        """Test empty pattern string."""
        result = _fastapi_native.extract_path_params("")
        assert result == []

    def test_root_only_pattern(self):
        """Test root path only."""
        result = _fastapi_native.extract_path_params("/")
        assert result == []

    def test_no_parameters(self):
        """Test pattern with no parameters."""
        result = _fastapi_native.extract_path_params("/api/v1/status")
        assert result == []

    def test_long_parameter_name(self):
        """Test very long parameter name."""
        long_name = "a" * 100
        result = _fastapi_native.extract_path_params(f"/items/{{{long_name}}}")
        assert result == [long_name]

    def test_many_parameters(self):
        """Test pattern with many parameters."""
        params = [f"param{i}" for i in range(10)]
        pattern = "/" + "/".join(f"{{{p}}}" for p in params)
        result = _fastapi_native.extract_path_params(pattern)
        assert result == params

    def test_mixed_static_and_params(self):
        """Test alternating static segments and parameters."""
        result = _fastapi_native.extract_path_params("/a/{b}/c/{d}/e/{f}")
        assert result == ["b", "d", "f"]

    def test_parameter_at_end(self):
        """Test parameter at end of path."""
        result = _fastapi_native.extract_path_params("/items/details/{id}")
        assert result == ["id"]

    def test_deep_nesting(self):
        """Test deeply nested path."""
        result = _fastapi_native.extract_path_params(
            "/api/v1/org/{org_id}/team/{team_id}/member/{member_id}/role/{role_id}"
        )
        assert result == ["org_id", "team_id", "member_id", "role_id"]

    def test_randomized_patterns(self):
        """Test with randomized patterns."""
        for _ in range(10):
            num_params = random.randint(1, 5)
            param_names = [random_string(8) for _ in range(num_params)]
            segments = []
            for i, name in enumerate(param_names):
                if random.random() > 0.5:
                    segments.append(random_string(5))
                segments.append(f"{{{name}}}")
            pattern = "/" + "/".join(segments)
            result = _fastapi_native.extract_path_params(pattern)
            assert result == param_names

    def test_empty_braces(self):
        """Test handling of empty braces."""
        result = _fastapi_native.extract_path_params("/items/{}")
        # Should either return empty string or be treated as literal
        assert isinstance(result, list)

    def test_single_char_parameter(self):
        """Test single character parameter name."""
        result = _fastapi_native.extract_path_params("/items/{x}")
        assert result == ["x"]


# =============================================================================
# Query Parameter Parsing Tests - Edge Cases
# =============================================================================

class TestQueryParamParsing:
    """Edge case tests for query parameter parsing."""

    def test_empty_query_string(self):
        """Test empty query string."""
        result = _fastapi_native.parse_query_params("/search")
        assert result == {}

    def test_query_string_only_question_mark(self):
        """Test URL with only question mark."""
        result = _fastapi_native.parse_query_params("/search?")
        assert result == {}

    def test_empty_value(self):
        """Test query param with empty value."""
        result = _fastapi_native.parse_query_params("/search?key=")
        assert result == {"key": ""}

    def test_key_without_equals(self):
        """Test query param without equals sign."""
        result = _fastapi_native.parse_query_params("/search?key")
        # Should handle gracefully - either empty string value or skip
        assert isinstance(result, dict)

    def test_empty_key(self):
        """Test query param with empty key."""
        result = _fastapi_native.parse_query_params("/search?=value")
        assert isinstance(result, dict)

    def test_duplicate_keys(self):
        """Test duplicate query keys (last value wins)."""
        result = _fastapi_native.parse_query_params("/search?key=first&key=second")
        # Per API docs, last value wins
        assert result.get("key") == "second"

    def test_many_parameters(self):
        """Test many query parameters."""
        params = {f"key{i}": f"value{i}" for i in range(20)}
        query = "&".join(f"{k}={v}" for k, v in params.items())
        result = _fastapi_native.parse_query_params(f"/search?{query}")
        assert result == params

    def test_special_characters_in_value(self):
        """Test special characters URL-encoded in value."""
        result = _fastapi_native.parse_query_params("/search?msg=hello%20world")
        assert result == {"msg": "hello world"}

    def test_plus_as_space(self):
        """Test plus sign decoded as space."""
        result = _fastapi_native.parse_query_params("/search?msg=hello+world")
        # Plus should be decoded as space
        assert result.get("msg") in ["hello world", "hello+world"]

    def test_unicode_value(self):
        """Test unicode characters URL-encoded."""
        # UTF-8 encoding of "日本語" is E6 97 A5 E6 9C AC E8 AA 9E
        result = _fastapi_native.parse_query_params(
            "/search?text=%E6%97%A5%E6%9C%AC%E8%AA%9E"
        )
        assert result.get("text") == "日本語"

    def test_ampersand_in_value(self):
        """Test URL-encoded ampersand in value."""
        result = _fastapi_native.parse_query_params("/search?text=a%26b")
        assert result == {"text": "a&b"}

    def test_equals_in_value(self):
        """Test URL-encoded equals in value."""
        result = _fastapi_native.parse_query_params("/search?eq=%3D")
        assert result == {"eq": "="}

    def test_percent_in_value(self):
        """Test URL-encoded percent sign."""
        result = _fastapi_native.parse_query_params("/search?pct=%25")
        assert result == {"pct": "%"}

    def test_long_value(self):
        """Test very long query parameter value."""
        long_value = "a" * 1000
        result = _fastapi_native.parse_query_params(f"/search?key={long_value}")
        assert result == {"key": long_value}

    def test_numeric_values(self):
        """Test numeric query parameter values."""
        result = _fastapi_native.parse_query_params(
            "/search?int=123&float=45.67&neg=-99"
        )
        assert result == {"int": "123", "float": "45.67", "neg": "-99"}

    def test_boolean_values(self):
        """Test boolean-like query parameter values."""
        result = _fastapi_native.parse_query_params(
            "/search?active=true&deleted=false&enabled=1&disabled=0"
        )
        assert result == {
            "active": "true",
            "deleted": "false",
            "enabled": "1",
            "disabled": "0"
        }

    def test_mixed_encoding(self):
        """Test mixed encoded and non-encoded characters."""
        result = _fastapi_native.parse_query_params(
            "/search?msg=Hello%20World%21%20%3A%29"
        )
        assert result == {"msg": "Hello World! :)"}

    def test_randomized_query_params(self):
        """Test with randomized query parameters."""
        for _ in range(10):
            num_params = random.randint(1, 10)
            params = {}
            for i in range(num_params):
                key = random_string(5)
                value = random_string(10)
                params[key] = value
            query = "&".join(f"{k}={v}" for k, v in params.items())
            result = _fastapi_native.parse_query_params(f"/search?{query}")
            assert result == params

    def test_path_with_query(self):
        """Test extracting query from full path."""
        result = _fastapi_native.parse_query_params(
            "/api/v1/users/123/posts?page=1&size=10"
        )
        assert result == {"page": "1", "size": "10"}


# =============================================================================
# URL Decoding Tests - Edge Cases
# =============================================================================

class TestUrlDecoding:
    """Edge case tests for URL decoding."""

    def test_no_encoding(self):
        """Test string with no encoding."""
        assert _fastapi_native.url_decode("hello") == "hello"

    def test_empty_string(self):
        """Test empty string."""
        assert _fastapi_native.url_decode("") == ""

    def test_space_encoding(self):
        """Test space encoding (%20)."""
        assert _fastapi_native.url_decode("hello%20world") == "hello world"

    def test_multiple_spaces(self):
        """Test multiple space encodings."""
        assert _fastapi_native.url_decode("a%20b%20c") == "a b c"

    def test_special_characters(self):
        """Test common special characters."""
        test_cases = [
            ("%21", "!"),
            ("%23", "#"),
            ("%24", "$"),
            ("%25", "%"),
            ("%26", "&"),
            ("%27", "'"),
            ("%28", "("),
            ("%29", ")"),
            ("%2A", "*"),
            ("%2B", "+"),
            ("%2C", ","),
            ("%2F", "/"),
            ("%3A", ":"),
            ("%3B", ";"),
            ("%3D", "="),
            ("%3F", "?"),
            ("%40", "@"),
            ("%5B", "["),
            ("%5D", "]"),
        ]
        for encoded, expected in test_cases:
            result = _fastapi_native.url_decode(encoded)
            assert result == expected, f"Expected '{expected}' for '{encoded}', got '{result}'"

    def test_lowercase_hex(self):
        """Test lowercase hex digits."""
        assert _fastapi_native.url_decode("%2f") == "/"
        assert _fastapi_native.url_decode("%3a") == ":"

    def test_uppercase_hex(self):
        """Test uppercase hex digits."""
        assert _fastapi_native.url_decode("%2F") == "/"
        assert _fastapi_native.url_decode("%3A") == ":"

    def test_mixed_case_hex(self):
        """Test mixed case hex digits."""
        assert _fastapi_native.url_decode("%2f%3A") == "/:"

    def test_unicode_utf8(self):
        """Test UTF-8 encoded unicode."""
        # Euro sign: €
        assert _fastapi_native.url_decode("%E2%82%AC") == "€"
        # Japanese: 日
        assert _fastapi_native.url_decode("%E6%97%A5") == "日"

    def test_emoji_utf8(self):
        """Test UTF-8 encoded emoji."""
        # Smiley face: 😀 = F0 9F 98 80
        result = _fastapi_native.url_decode("%F0%9F%98%80")
        assert result == "😀"

    def test_incomplete_percent_encoding(self):
        """Test handling of incomplete percent encoding."""
        # Single % at end
        result = _fastapi_native.url_decode("hello%")
        # Should handle gracefully
        assert isinstance(result, str)

        # % with only one hex digit
        result = _fastapi_native.url_decode("hello%2")
        assert isinstance(result, str)

    def test_invalid_hex_characters(self):
        """Test handling of invalid hex characters."""
        # %GG is not valid hex
        result = _fastapi_native.url_decode("hello%GG")
        assert isinstance(result, str)

        # %ZZ is not valid hex
        result = _fastapi_native.url_decode("hello%ZZ")
        assert isinstance(result, str)

    def test_double_encoding(self):
        """Test double-encoded strings (decoded once)."""
        # %2520 = %25 + 20 = "%" + "20" after first decode = "%20"
        result = _fastapi_native.url_decode("%2520")
        assert result == "%20"  # Only one level of decoding

    def test_long_encoded_string(self):
        """Test long encoded string."""
        # Create a long string of spaces
        encoded = "%20" * 100
        result = _fastapi_native.url_decode(encoded)
        assert result == " " * 100

    def test_null_byte(self):
        """Test null byte encoding."""
        result = _fastapi_native.url_decode("hello%00world")
        # Null byte handling varies - just ensure no crash
        assert isinstance(result, str)

    def test_high_bytes(self):
        """Test high byte values - may raise UnicodeDecodeError for invalid UTF-8."""
        try:
            result = _fastapi_native.url_decode("%FF")
            # If it doesn't raise, it should return something
            assert isinstance(result, str)
        except UnicodeDecodeError:
            # This is expected - 0xFF is not valid UTF-8 start byte
            pass

    def test_all_encoded_string(self):
        """Test string that is entirely encoded."""
        result = _fastapi_native.url_decode("%48%65%6C%6C%6F")
        assert result == "Hello"

    def test_randomized_encoding(self):
        """Test with randomized encoded strings."""
        for _ in range(10):
            original = random_string(20)
            encoded = generate_url_encoded_string(original)
            result = _fastapi_native.url_decode(encoded)
            assert result == original


# =============================================================================
# Combined Path and Query Tests - Edge Cases
# =============================================================================

class TestCombinedPathQuery:
    """Tests for combined path and query parameter edge cases."""

    def test_path_with_empty_query(self):
        """Test path with empty query string."""
        path_params = _fastapi_native.extract_path_params("/items/{id}")
        query_params = _fastapi_native.parse_query_params("/items/123?")

        assert path_params == ["id"]
        assert query_params == {}

    def test_complex_url(self):
        """Test complex URL with multiple segments and query params."""
        pattern = "/api/v1/users/{user_id}/posts/{post_id}"
        path_params = _fastapi_native.extract_path_params(pattern)

        query = "/api/v1/users/42/posts/789?page=1&size=10&sort=desc&filter=active"
        query_params = _fastapi_native.parse_query_params(query)

        assert path_params == ["user_id", "post_id"]
        assert query_params == {
            "page": "1",
            "size": "10",
            "sort": "desc",
            "filter": "active"
        }

    def test_encoded_path_segment(self):
        """Test URL-encoded characters in path that would be query params."""
        # Path with ? in it (encoded as %3F)
        query = "/search?q=what%3Fis%3Fthis"
        params = _fastapi_native.parse_query_params(query)
        assert params == {"q": "what?is?this"}

    def test_hash_fragment_ignored(self):
        """Test that hash fragments are handled (typically ignored in query)."""
        # Hash fragments should not affect query parsing
        query = "/search?q=test#section1"
        params = _fastapi_native.parse_query_params(query)
        # Implementation may or may not strip fragment
        assert "q" in params


# =============================================================================
# Stress Tests
# =============================================================================

class TestParameterStress:
    """Stress tests for parameter extraction."""

    def test_very_long_path(self):
        """Test very long path with many segments."""
        segments = [random_string(10) for _ in range(100)]
        path = "/" + "/".join(segments)
        result = _fastapi_native.extract_path_params(path)
        assert result == []  # No parameters in this path

    def test_very_long_query_string(self):
        """Test very long query string."""
        params = {random_string(8): random_string(20) for _ in range(100)}
        query = "&".join(f"{k}={v}" for k, v in params.items())
        result = _fastapi_native.parse_query_params(f"/search?{query}")
        assert result == params

    def test_rapid_extraction(self):
        """Test rapid repeated extractions."""
        pattern = "/users/{user_id}/posts/{post_id}"
        for _ in range(1000):
            result = _fastapi_native.extract_path_params(pattern)
            assert result == ["user_id", "post_id"]

    def test_rapid_query_parsing(self):
        """Test rapid repeated query parsing."""
        url = "/search?q=test&limit=10&offset=0"
        expected = {"q": "test", "limit": "10", "offset": "0"}
        for _ in range(1000):
            result = _fastapi_native.parse_query_params(url)
            assert result == expected

    def test_rapid_url_decoding(self):
        """Test rapid repeated URL decoding."""
        encoded = "hello%20world%21"
        for _ in range(1000):
            result = _fastapi_native.url_decode(encoded)
            assert result == "hello world!"


# =============================================================================
# Type Coercion Edge Cases
# =============================================================================

class TestTypeEdgeCases:
    """Edge cases for type handling."""

    def test_numeric_string_values(self):
        """Test that numeric values are returned as strings."""
        result = _fastapi_native.parse_query_params("/api?id=123")
        assert result["id"] == "123"
        assert isinstance(result["id"], str)

    def test_float_string_values(self):
        """Test that float values are returned as strings."""
        result = _fastapi_native.parse_query_params("/api?price=19.99")
        assert result["price"] == "19.99"
        assert isinstance(result["price"], str)

    def test_scientific_notation(self):
        """Test scientific notation values."""
        result = _fastapi_native.parse_query_params("/api?val=1e10")
        assert result["val"] == "1e10"

    def test_negative_numbers(self):
        """Test negative number values."""
        result = _fastapi_native.parse_query_params("/api?temp=-42")
        assert result["temp"] == "-42"

    def test_zero_values(self):
        """Test zero values."""
        result = _fastapi_native.parse_query_params("/api?count=0&offset=00")
        assert result == {"count": "0", "offset": "00"}


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
