"""
Test the new parameter extraction utility functions added to _fastapi_native.
"""

import sys
sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi import _fastapi_native

def test_extract_path_params():
    """Test extracting path parameter names from route patterns."""
    print("Testing extract_path_params...")

    # Test single parameter
    result1 = _fastapi_native.extract_path_params("/users/{user_id}")
    assert result1 == ["user_id"], f"Expected ['user_id'], got {result1}"
    print(f"✓ Single param: {result1}")

    # Test multiple parameters
    result2 = _fastapi_native.extract_path_params("/api/{version}/items/{item_id}")
    assert result2 == ["version", "item_id"], f"Expected ['version', 'item_id'], got {result2}"
    print(f"✓ Multiple params: {result2}")

    # Test no parameters
    result3 = _fastapi_native.extract_path_params("/api/status")
    assert result3 == [], f"Expected [], got {result3}"
    print(f"✓ No params: {result3}")

    print("✓ extract_path_params tests passed!\n")


def test_parse_query_params():
    """Test parsing query parameters from URLs."""
    print("Testing parse_query_params...")

    # Test single query param
    result1 = _fastapi_native.parse_query_params("/search?q=test")
    assert result1 == {"q": "test"}, f"Expected {{'q': 'test'}}, got {result1}"
    print(f"✓ Single query: {result1}")

    # Test multiple query params
    result2 = _fastapi_native.parse_query_params("/search?q=hello&limit=50&offset=10")
    expected2 = {"q": "hello", "limit": "50", "offset": "10"}
    assert result2 == expected2, f"Expected {expected2}, got {result2}"
    print(f"✓ Multiple queries: {result2}")

    # Test no query params
    result3 = _fastapi_native.parse_query_params("/api/users")
    assert result3 == {}, f"Expected {{}}, got {result3}"
    print(f"✓ No queries: {result3}")

    # Test URL-encoded values
    result4 = _fastapi_native.parse_query_params("/search?q=hello%20world")
    assert result4 == {"q": "hello world"}, f"Expected {{'q': 'hello world'}}, got {result4}"
    print(f"✓ URL-encoded: {result4}")

    print("✓ parse_query_params tests passed!\n")


def test_url_decode():
    """Test URL decoding."""
    print("Testing url_decode...")

    # Test basic decoding
    result1 = _fastapi_native.url_decode("hello%20world")
    assert result1 == "hello world", f"Expected 'hello world', got '{result1}'"
    print(f"✓ Space: '{result1}'")

    # Test special characters
    result2 = _fastapi_native.url_decode("user%40example.com")
    assert result2 == "user@example.com", f"Expected 'user@example.com', got '{result2}'"
    print(f"✓ @ symbol: '{result2}'")

    # Test plus sign
    result3 = _fastapi_native.url_decode("hello%2Bworld")
    assert result3 == "hello+world", f"Expected 'hello+world', got '{result3}'"
    print(f"✓ + symbol: '{result3}'")

    # Test no encoding
    result4 = _fastapi_native.url_decode("hello")
    assert result4 == "hello", f"Expected 'hello', got '{result4}'"
    print(f"✓ No encoding: '{result4}'")

    print("✓ url_decode tests passed!\n")


if __name__ == "__main__":
    print("="*60)
    print("Testing ParameterExtractor Cython bindings")
    print("="*60 + "\n")

    try:
        test_extract_path_params()
        test_parse_query_params()
        test_url_decode()

        print("="*60)
        print("ALL TESTS PASSED!")
        print("="*60)
    except Exception as e:
        print(f"\n✗ Test failed: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
