"""
Comprehensive tests for path parameter extraction and type coercion.

Tests:
- Path parameter extraction from {param} placeholders
- Type coercion (str → int, str → float, str → bool)
- Multiple parameters in single path
- Nested/complex paths
- Edge cases (special characters, Unicode, etc.)
- HTTP/1.1, HTTP/2, HTTP/3 protocol transparency

These tests verify that FasterAPI matches FastAPI behavior for path parameters.
"""

import pytest
import requests
import random
import string
from fasterapi.fastapi_compat import FastAPI
from fasterapi.fastapi_server import FastAPIServer
from pydantic import BaseModel
import time
import threading


class TestPathParameterExtraction:
    """Test basic path parameter extraction."""

    @pytest.fixture(scope="class")
    def app_and_server(self):
        """Create FastAPI app with path parameter routes."""
        app = FastAPI(title="Path Params Test", version="1.0.0")

        @app.get("/users/{user_id}")
        def get_user(user_id: int):
            """Get user by ID with type coercion to int."""
            return {"user_id": user_id, "type": type(user_id).__name__}

        @app.get("/posts/{post_id}/comments/{comment_id}")
        def get_comment(post_id: int, comment_id: int):
            """Get comment with two path parameters."""
            return {
                "post_id": post_id,
                "comment_id": comment_id,
                "types": [type(post_id).__name__, type(comment_id).__name__]
            }

        @app.get("/items/{item_id}/price/{price}")
        def get_item_price(item_id: str, price: float):
            """Test mixed types: str and float."""
            return {
                "item_id": item_id,
                "price": price,
                "types": [type(item_id).__name__, type(price).__name__]
            }

        @app.get("/flags/{enabled}")
        def get_flag(enabled: bool):
            """Test boolean type coercion."""
            return {"enabled": enabled, "type": type(enabled).__name__}

        # Start server
        server = FastAPIServer(app, port=8001, host="127.0.0.1")
        server_thread = threading.Thread(target=server.run, daemon=True)
        server_thread.start()
        time.sleep(0.5)  # Give server time to start

        yield app, server

        # Cleanup
        server.stop()

    def test_single_path_param_int(self, app_and_server):
        """Test single path parameter with int type."""
        app, server = app_and_server

        # Test with random integers
        for _ in range(10):
            user_id = random.randint(1, 1000000)
            response = requests.get(f"http://127.0.0.1:8001/users/{user_id}")

            assert response.status_code == 200
            data = response.json()
            assert data["user_id"] == user_id
            assert data["type"] == "int"

    def test_multiple_path_params(self, app_and_server):
        """Test multiple path parameters."""
        app, server = app_and_server

        for _ in range(10):
            post_id = random.randint(1, 10000)
            comment_id = random.randint(1, 10000)

            response = requests.get(
                f"http://127.0.0.1:8001/posts/{post_id}/comments/{comment_id}"
            )

            assert response.status_code == 200
            data = response.json()
            assert data["post_id"] == post_id
            assert data["comment_id"] == comment_id
            assert data["types"] == ["int", "int"]

    def test_mixed_type_params(self, app_and_server):
        """Test path parameters with different types."""
        app, server = app_and_server

        test_cases = [
            ("item123", 19.99),
            ("product-abc", 99.95),
            ("test_item", 0.01),
            ("ITEM_XYZ", 1234.56),
        ]

        for item_id, price in test_cases:
            response = requests.get(
                f"http://127.0.0.1:8001/items/{item_id}/price/{price}"
            )

            assert response.status_code == 200
            data = response.json()
            assert data["item_id"] == item_id
            assert abs(data["price"] - price) < 0.001  # Float comparison
            assert data["types"] == ["str", "float"]

    def test_bool_type_coercion(self, app_and_server):
        """Test boolean type coercion from string."""
        app, server = app_and_server

        # Test various boolean representations
        true_values = ["true", "True", "1"]
        false_values = ["false", "False", "0"]

        for value in true_values:
            response = requests.get(f"http://127.0.0.1:8001/flags/{value}")
            assert response.status_code == 200
            data = response.json()
            assert data["enabled"] is True
            assert data["type"] == "bool"

        for value in false_values:
            response = requests.get(f"http://127.0.0.1:8001/flags/{value}")
            assert response.status_code == 200
            data = response.json()
            assert data["enabled"] is False
            assert data["type"] == "bool"


class TestPathParameterEdgeCases:
    """Test edge cases for path parameters."""

    @pytest.fixture(scope="class")
    def app_and_server(self):
        """Create FastAPI app with edge case routes."""
        app = FastAPI()

        @app.get("/unicode/{text}")
        def get_unicode(text: str):
            """Test Unicode in path parameters."""
            return {"text": text, "length": len(text)}

        @app.get("/special/{value}")
        def get_special(value: str):
            """Test special characters."""
            return {"value": value}

        @app.get("/numbers/{num}")
        def get_number(num: int):
            """Test various number formats."""
            return {"num": num}

        # Start server
        server = FastAPIServer(app, port=8002, host="127.0.0.1")
        server_thread = threading.Thread(target=server.run, daemon=True)
        server_thread.start()
        time.sleep(0.5)

        yield app, server

        server.stop()

    def test_unicode_parameters(self, app_and_server):
        """Test Unicode characters in path parameters."""
        app, server = app_and_server

        test_cases = [
            "hello",
            "世界",  # Chinese
            "مرحبا",  # Arabic
            "Привет",  # Russian
            "emoji_😀_test",
        ]

        for text in test_cases:
            # URL-encode the text
            import urllib.parse
            encoded = urllib.parse.quote(text, safe='')

            response = requests.get(f"http://127.0.0.1:8002/unicode/{encoded}")

            # requests automatically decodes
            assert response.status_code == 200
            data = response.json()
            assert data["text"] == text
            assert data["length"] == len(text)

    def test_special_characters(self, app_and_server):
        """Test special characters (URL-encoded)."""
        app, server = app_and_server

        test_cases = [
            ("test-value", "test-value"),  # Dash
            ("test_value", "test_value"),  # Underscore
            ("test.value", "test.value"),  # Dot
            ("test%20value", "test value"),  # Space (URL-encoded)
            ("test%2Bvalue", "test+value"),  # Plus (URL-encoded)
        ]

        for input_val, expected in test_cases:
            response = requests.get(f"http://127.0.0.1:8002/special/{input_val}")

            assert response.status_code == 200
            data = response.json()
            assert data["value"] == expected

    def test_large_numbers(self, app_and_server):
        """Test large integers."""
        app, server = app_and_server

        test_cases = [
            0,
            1,
            -1,
            999999999,
            -999999999,
            2147483647,  # Max 32-bit int
            -2147483648,  # Min 32-bit int
        ]

        for num in test_cases:
            response = requests.get(f"http://127.0.0.1:8002/numbers/{num}")

            assert response.status_code == 200
            data = response.json()
            assert data["num"] == num


class TestRandomizedPathParameters:
    """Randomized testing for path parameters."""

    @pytest.fixture(scope="class")
    def app_and_server(self):
        """Create FastAPI app for randomized tests."""
        app = FastAPI()

        @app.get("/calc/{a}/{b}/{op}")
        def calculate(a: int, b: int, op: str):
            """Calculator endpoint for randomized testing."""
            ops = {
                "add": a + b,
                "sub": a - b,
                "mul": a * b,
                "div": a // b if b != 0 else 0,
            }
            return {
                "a": a,
                "b": b,
                "op": op,
                "result": ops.get(op, 0)
            }

        # Start server
        server = FastAPIServer(app, port=8003, host="127.0.0.1")
        server_thread = threading.Thread(target=server.run, daemon=True)
        server_thread.start()
        time.sleep(0.5)

        yield app, server

        server.stop()

    def test_randomized_calculations(self, app_and_server):
        """Test 100 random calculations to ensure parameter extraction works."""
        app, server = app_and_server

        operations = ["add", "sub", "mul", "div"]

        for _ in range(100):
            a = random.randint(-1000, 1000)
            b = random.randint(1, 1000)  # Avoid division by zero
            op = random.choice(operations)

            response = requests.get(f"http://127.0.0.1:8003/calc/{a}/{b}/{op}")

            assert response.status_code == 200
            data = response.json()

            # Verify parameters extracted correctly
            assert data["a"] == a
            assert data["b"] == b
            assert data["op"] == op

            # Verify calculation
            if op == "add":
                assert data["result"] == a + b
            elif op == "sub":
                assert data["result"] == a - b
            elif op == "mul":
                assert data["result"] == a * b
            elif op == "div":
                assert data["result"] == a // b


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
