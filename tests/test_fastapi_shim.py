#!/usr/bin/env python3
"""
FastAPI Shim Unit Tests

Tests the FastAPI compatibility layer (fasterapi/fastapi_compat.py) without
requiring a running server or the native C++ bindings.

These tests focus on:
- Type mapping from Python types to schema strings
- Parameter extraction from function signatures
- Pydantic schema extraction
- FastAPIApp class behavior
- Parameter location detection (path vs query vs body)
- Optional/Union type handling
- Decorator behavior

This enables us to verify FastAPI code compatibility before integration testing.
"""

import sys
import inspect
import random
import string
from typing import Any, Optional, List, Dict, Union
from dataclasses import dataclass
from unittest.mock import patch, MagicMock

import pytest

sys.path.insert(0, '/Users/bengamble/FasterAPI')

# Import the shim components
from fasterapi.fastapi_compat import (
    python_type_to_string,
    extract_function_parameters,
    PYTHON_TYPE_MAP,
)

# Check for Pydantic
try:
    from pydantic import BaseModel, Field
    from fasterapi.fastapi_compat import extract_pydantic_schema
    HAS_PYDANTIC = True
except ImportError:
    HAS_PYDANTIC = False
    BaseModel = None

# Check for native bindings
try:
    from fasterapi._fastapi_native import register_route
    HAS_NATIVE = True
except ImportError:
    HAS_NATIVE = False


# =============================================================================
# Test Helpers
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate a random alphanumeric string that is a valid Python identifier."""
    # First char must be letter or underscore (Python identifier rules)
    first = random.choice(string.ascii_letters)
    if length == 1:
        return first
    rest = ''.join(random.choices(string.ascii_letters + string.digits, k=length - 1))
    return first + rest


def random_int(min_val: int = 1, max_val: int = 1000) -> int:
    """Generate a random integer."""
    return random.randint(min_val, max_val)


# Skip markers
requires_pydantic = pytest.mark.skipif(
    not HAS_PYDANTIC,
    reason="Pydantic not available"
)

requires_native = pytest.mark.skipif(
    not HAS_NATIVE,
    reason="Native bindings not available"
)


# =============================================================================
# Type Mapping Tests
# =============================================================================

class TestPythonTypeToString:
    """Tests for python_type_to_string function."""

    def test_basic_string_type(self):
        """Test str type mapping."""
        assert python_type_to_string(str) == 'string'

    def test_basic_int_type(self):
        """Test int type mapping."""
        assert python_type_to_string(int) == 'integer'

    def test_basic_float_type(self):
        """Test float type mapping."""
        assert python_type_to_string(float) == 'float'

    def test_basic_bool_type(self):
        """Test bool type mapping."""
        assert python_type_to_string(bool) == 'boolean'

    def test_basic_list_type(self):
        """Test list type mapping."""
        assert python_type_to_string(list) == 'array'

    def test_basic_dict_type(self):
        """Test dict type mapping."""
        assert python_type_to_string(dict) == 'object'

    def test_none_type(self):
        """Test None type mapping."""
        assert python_type_to_string(type(None)) == 'null'

    def test_generic_list(self):
        """Test List[T] generic type."""
        assert python_type_to_string(List[int]) == 'array'
        assert python_type_to_string(List[str]) == 'array'
        assert python_type_to_string(List[float]) == 'array'

    def test_generic_dict(self):
        """Test Dict[K, V] generic type."""
        assert python_type_to_string(Dict[str, int]) == 'object'
        assert python_type_to_string(Dict[str, Any]) == 'object'

    def test_optional_type(self):
        """Test Optional[T] type handling."""
        assert python_type_to_string(Optional[str]) == 'string'
        assert python_type_to_string(Optional[int]) == 'integer'
        assert python_type_to_string(Optional[float]) == 'float'
        assert python_type_to_string(Optional[bool]) == 'boolean'

    def test_union_with_none(self):
        """Test Union[T, None] (same as Optional)."""
        assert python_type_to_string(Union[str, None]) == 'string'
        assert python_type_to_string(Union[int, None]) == 'integer'
        # Order shouldn't matter
        assert python_type_to_string(Union[None, str]) == 'string'

    def test_any_type(self):
        """Test Any type defaults to 'any'."""
        assert python_type_to_string(Any) == 'any'

    def test_unknown_type(self):
        """Test unknown types default to 'any'."""
        class CustomClass:
            pass
        assert python_type_to_string(CustomClass) == 'any'

    def test_nested_optional_list(self):
        """Test Optional[List[T]] type."""
        # Should resolve to 'array' (the non-None type)
        assert python_type_to_string(Optional[List[int]]) == 'array'

    def test_nested_optional_dict(self):
        """Test Optional[Dict[K, V]] type."""
        assert python_type_to_string(Optional[Dict[str, int]]) == 'object'

    def test_all_python_type_map_entries(self):
        """Test all entries in PYTHON_TYPE_MAP are correct."""
        expected = {
            str: 'string',
            int: 'integer',
            float: 'float',
            bool: 'boolean',
            list: 'array',
            dict: 'object',
            type(None): 'null',
        }
        for py_type, schema_type in expected.items():
            assert PYTHON_TYPE_MAP.get(py_type) == schema_type
            assert python_type_to_string(py_type) == schema_type

    def test_randomized_type_consistency(self):
        """Test type mapping is consistent across multiple calls."""
        types_to_test = [str, int, float, bool, list, dict]
        for _ in range(100):
            t = random.choice(types_to_test)
            result1 = python_type_to_string(t)
            result2 = python_type_to_string(t)
            assert result1 == result2

    @requires_pydantic
    def test_pydantic_model_type(self):
        """Test Pydantic BaseModel maps to 'object'."""
        class UserModel(BaseModel):
            name: str
            age: int

        assert python_type_to_string(UserModel) == 'object'


# =============================================================================
# Parameter Extraction Tests
# =============================================================================

class TestExtractFunctionParameters:
    """Tests for extract_function_parameters function."""

    def test_simple_path_parameter(self):
        """Test extracting a single path parameter."""
        def get_user(user_id: int):
            pass

        params = extract_function_parameters(get_user, '/users/{user_id}', 'GET')
        assert len(params) == 1
        assert params[0]['name'] == 'user_id'
        assert params[0]['type'] == 'integer'
        assert params[0]['location'] == 'path'
        assert params[0]['required'] is True

    def test_multiple_path_parameters(self):
        """Test extracting multiple path parameters."""
        def get_item(org_id: str, item_id: int):
            pass

        params = extract_function_parameters(
            get_item, '/orgs/{org_id}/items/{item_id}', 'GET'
        )
        assert len(params) == 2

        org_param = next(p for p in params if p['name'] == 'org_id')
        assert org_param['type'] == 'string'
        assert org_param['location'] == 'path'
        assert org_param['required'] is True

        item_param = next(p for p in params if p['name'] == 'item_id')
        assert item_param['type'] == 'integer'
        assert item_param['location'] == 'path'
        assert item_param['required'] is True

    def test_query_parameter_required(self):
        """Test required query parameter (no default)."""
        def search(query: str):
            pass

        params = extract_function_parameters(search, '/search', 'GET')
        assert len(params) == 1
        assert params[0]['name'] == 'query'
        assert params[0]['type'] == 'string'
        assert params[0]['location'] == 'query'
        assert params[0]['required'] is True

    def test_query_parameter_optional(self):
        """Test optional query parameter (has default)."""
        def search(query: str = ""):
            pass

        params = extract_function_parameters(search, '/search', 'GET')
        assert len(params) == 1
        assert params[0]['name'] == 'query'
        assert params[0]['location'] == 'query'
        assert params[0]['required'] is False
        assert params[0]['default'] == ''

    def test_query_parameter_with_default_value(self):
        """Test query parameter with non-empty default."""
        def list_items(page: int = 1, limit: int = 10):
            pass

        params = extract_function_parameters(list_items, '/items', 'GET')
        assert len(params) == 2

        page_param = next(p for p in params if p['name'] == 'page')
        assert page_param['type'] == 'integer'
        assert page_param['location'] == 'query'
        assert page_param['required'] is False
        assert page_param['default'] == '1'

        limit_param = next(p for p in params if p['name'] == 'limit')
        assert limit_param['default'] == '10'

    def test_mixed_path_and_query(self):
        """Test function with both path and query parameters."""
        def get_user_items(user_id: int, status: str = 'active'):
            pass

        params = extract_function_parameters(
            get_user_items, '/users/{user_id}/items', 'GET'
        )
        assert len(params) == 2

        user_param = next(p for p in params if p['name'] == 'user_id')
        assert user_param['location'] == 'path'
        assert user_param['required'] is True

        status_param = next(p for p in params if p['name'] == 'status')
        assert status_param['location'] == 'query'
        assert status_param['required'] is False

    def test_no_parameters(self):
        """Test function with no parameters."""
        def health_check():
            pass

        params = extract_function_parameters(health_check, '/health', 'GET')
        assert params == []

    def test_parameter_without_type_hint(self):
        """Test parameter without type annotation defaults to 'any'."""
        def legacy_handler(data):
            pass

        params = extract_function_parameters(legacy_handler, '/legacy', 'POST')
        assert len(params) == 1
        assert params[0]['name'] == 'data'
        assert params[0]['type'] == 'any'

    def test_optional_type_parameter(self):
        """Test Optional type in parameter."""
        def search(query: Optional[str] = None):
            pass

        params = extract_function_parameters(search, '/search', 'GET')
        assert len(params) == 1
        assert params[0]['name'] == 'query'
        assert params[0]['type'] == 'string'  # Optional unwraps to inner type
        assert params[0]['required'] is False

    def test_list_type_parameter(self):
        """Test List type parameter."""
        def filter_items(ids: List[int]):
            pass

        params = extract_function_parameters(filter_items, '/filter', 'POST')
        assert len(params) == 1
        assert params[0]['name'] == 'ids'
        assert params[0]['type'] == 'array'

    def test_dict_type_parameter(self):
        """Test Dict type parameter."""
        def update_config(settings: Dict[str, Any]):
            pass

        params = extract_function_parameters(update_config, '/config', 'PUT')
        assert len(params) == 1
        assert params[0]['name'] == 'settings'
        assert params[0]['type'] == 'object'

    @requires_pydantic
    def test_pydantic_body_parameter(self):
        """Test Pydantic model is detected as body parameter."""
        class CreateUserRequest(BaseModel):
            name: str
            email: str

        def create_user(user: CreateUserRequest):
            pass

        params = extract_function_parameters(create_user, '/users', 'POST')
        assert len(params) == 1
        assert params[0]['name'] == 'user'
        assert params[0]['type'] == 'object'
        assert params[0]['location'] == 'body'
        assert params[0]['required'] is True

    @requires_pydantic
    def test_path_and_body_combined(self):
        """Test combination of path param and Pydantic body."""
        class UpdateUserRequest(BaseModel):
            name: str
            email: Optional[str] = None

        def update_user(user_id: int, user: UpdateUserRequest):
            pass

        params = extract_function_parameters(
            update_user, '/users/{user_id}', 'PUT'
        )
        assert len(params) == 2

        user_id_param = next(p for p in params if p['name'] == 'user_id')
        assert user_id_param['location'] == 'path'
        assert user_id_param['type'] == 'integer'

        user_param = next(p for p in params if p['name'] == 'user')
        assert user_param['location'] == 'body'
        assert user_param['type'] == 'object'

    def test_boolean_default_false(self):
        """Test boolean parameter with False default."""
        def toggle(active: bool = False):
            pass

        params = extract_function_parameters(toggle, '/toggle', 'POST')
        assert len(params) == 1
        assert params[0]['name'] == 'active'
        assert params[0]['type'] == 'boolean'
        assert params[0]['required'] is False
        assert params[0]['default'] == 'False'

    def test_boolean_default_true(self):
        """Test boolean parameter with True default."""
        def toggle(active: bool = True):
            pass

        params = extract_function_parameters(toggle, '/toggle', 'POST')
        assert params[0]['default'] == 'True'

    def test_complex_path_pattern(self):
        """Test complex nested path pattern."""
        def nested_resource(
            org: str,
            project: str,
            resource: int,
            version: str = 'latest'
        ):
            pass

        path = '/orgs/{org}/projects/{project}/resources/{resource}'
        params = extract_function_parameters(nested_resource, path, 'GET')
        assert len(params) == 4

        # Path params
        for name in ['org', 'project', 'resource']:
            param = next(p for p in params if p['name'] == name)
            assert param['location'] == 'path'
            assert param['required'] is True

        # Query param
        version_param = next(p for p in params if p['name'] == 'version')
        assert version_param['location'] == 'query'
        assert version_param['required'] is False

    def test_float_parameter(self):
        """Test float type parameter."""
        def set_value(amount: float):
            pass

        params = extract_function_parameters(set_value, '/value', 'POST')
        assert params[0]['type'] == 'float'

    def test_randomized_path_params(self):
        """Test with randomized path parameter names."""
        for _ in range(20):
            param_name = random_string(8)
            param_code = f"def handler({param_name}: int): pass"
            local_ns = {}
            exec(param_code, {}, local_ns)
            handler = local_ns['handler']

            path = f'/resource/{{{param_name}}}'
            params = extract_function_parameters(handler, path, 'GET')
            assert len(params) == 1
            assert params[0]['name'] == param_name
            assert params[0]['location'] == 'path'


# =============================================================================
# Pydantic Schema Extraction Tests
# =============================================================================

@requires_pydantic
class TestExtractPydanticSchema:
    """Tests for extract_pydantic_schema function."""

    def test_simple_model(self):
        """Test extracting schema from simple model."""
        class User(BaseModel):
            name: str
            age: int

        schema = extract_pydantic_schema(User)
        assert schema['name'] == 'User'
        assert len(schema['fields']) == 2

        name_field = next(f for f in schema['fields'] if f['name'] == 'name')
        assert name_field['type'] == 'string'
        assert name_field['required'] is True

        age_field = next(f for f in schema['fields'] if f['name'] == 'age')
        assert age_field['type'] == 'integer'
        assert age_field['required'] is True

    def test_model_with_optional_field(self):
        """Test model with optional field."""
        class Profile(BaseModel):
            username: str
            bio: Optional[str] = None

        schema = extract_pydantic_schema(Profile)
        assert len(schema['fields']) == 2

        bio_field = next(f for f in schema['fields'] if f['name'] == 'bio')
        assert bio_field['type'] == 'string'
        assert bio_field['required'] is False

    def test_model_with_default_value(self):
        """Test model with non-None default value."""
        class Settings(BaseModel):
            theme: str = 'light'
            page_size: int = 10

        schema = extract_pydantic_schema(Settings)

        theme_field = next(f for f in schema['fields'] if f['name'] == 'theme')
        assert theme_field['required'] is False
        assert theme_field['default'] == 'light'

        page_size_field = next(f for f in schema['fields'] if f['name'] == 'page_size')
        assert page_size_field['default'] == '10'

    def test_model_with_all_types(self):
        """Test model with various types."""
        class AllTypes(BaseModel):
            text: str
            number: int
            decimal: float
            flag: bool
            items: List[str]
            metadata: Dict[str, Any]

        schema = extract_pydantic_schema(AllTypes)
        assert len(schema['fields']) == 6

        type_mapping = {
            'text': 'string',
            'number': 'integer',
            'decimal': 'float',
            'flag': 'boolean',
            'items': 'array',
            'metadata': 'object',
        }
        for field in schema['fields']:
            assert field['type'] == type_mapping[field['name']]

    def test_custom_schema_name(self):
        """Test providing custom schema name."""
        class InternalModel(BaseModel):
            value: int

        schema = extract_pydantic_schema(InternalModel, schema_name='PublicModel')
        assert schema['name'] == 'PublicModel'

    def test_nested_model(self):
        """Test model with nested model field."""
        class Address(BaseModel):
            street: str
            city: str

        class Person(BaseModel):
            name: str
            address: Address

        schema = extract_pydantic_schema(Person)
        assert len(schema['fields']) == 2

        # Nested model should be 'object'
        address_field = next(f for f in schema['fields'] if f['name'] == 'address')
        assert address_field['type'] == 'object'

    def test_model_with_field_description(self):
        """Test model with Field descriptions."""
        class Product(BaseModel):
            name: str = Field(description="Product name")
            price: float = Field(gt=0, description="Product price")

        schema = extract_pydantic_schema(Product)
        # Fields should still be extracted
        assert len(schema['fields']) == 2

    def test_empty_model(self):
        """Test model with no fields."""
        class EmptyModel(BaseModel):
            pass

        schema = extract_pydantic_schema(EmptyModel)
        assert schema['name'] == 'EmptyModel'
        assert schema['fields'] == []

    def test_model_inheritance(self):
        """Test model that inherits from another model."""
        class Base(BaseModel):
            id: int

        class Extended(Base):
            name: str
            description: Optional[str] = None

        schema = extract_pydantic_schema(Extended)
        assert schema['name'] == 'Extended'
        # Should include inherited field
        field_names = {f['name'] for f in schema['fields']}
        assert 'id' in field_names
        assert 'name' in field_names
        assert 'description' in field_names

    def test_non_pydantic_returns_empty(self):
        """Test that non-Pydantic class returns empty fields."""
        class RegularClass:
            name: str

        schema = extract_pydantic_schema(RegularClass)
        assert schema['fields'] == []


# =============================================================================
# FastAPIApp Tests
# =============================================================================

class TestFastAPIApp:
    """Tests for FastAPIApp class without native bindings."""

    def test_app_initialization_defaults(self):
        """Test default initialization values."""
        # Mock native imports to test initialization logic
        with patch.dict('sys.modules', {'fasterapi._fastapi_native': MagicMock()}):
            # Re-import with mock
            import importlib
            import fasterapi.fastapi_compat as compat
            original_native = compat.HAS_NATIVE
            compat.HAS_NATIVE = False

            try:
                from fasterapi.fastapi_compat import FastAPIApp
                app = FastAPIApp()

                assert app.title == "FasterAPI"
                assert app.version == "0.1.0"
                assert app.description == ""
                assert app.docs_url == "/docs"
                assert app.redoc_url == "/redoc"
                assert app.openapi_url == "/openapi.json"
            finally:
                compat.HAS_NATIVE = original_native

    def test_app_custom_initialization(self):
        """Test custom initialization values."""
        import fasterapi.fastapi_compat as compat
        original_native = compat.HAS_NATIVE
        compat.HAS_NATIVE = False

        try:
            from fasterapi.fastapi_compat import FastAPIApp
            app = FastAPIApp(
                title="My API",
                version="2.0.0",
                description="My awesome API",
                docs_url="/swagger",
                redoc_url="/redoc-docs",
                openapi_url="/api/openapi.json"
            )

            assert app.title == "My API"
            assert app.version == "2.0.0"
            assert app.description == "My awesome API"
            assert app.docs_url == "/swagger"
            assert app.redoc_url == "/redoc-docs"
            assert app.openapi_url == "/api/openapi.json"
        finally:
            compat.HAS_NATIVE = original_native

    def test_app_disable_docs(self):
        """Test disabling documentation URLs."""
        import fasterapi.fastapi_compat as compat
        original_native = compat.HAS_NATIVE
        compat.HAS_NATIVE = False

        try:
            from fasterapi.fastapi_compat import FastAPIApp
            app = FastAPIApp(
                docs_url=None,
                redoc_url=None,
                openapi_url=None
            )

            assert app.docs_url is None
            assert app.redoc_url is None
            assert app.openapi_url is None
        finally:
            compat.HAS_NATIVE = original_native


class TestFastAPIAppDecorators:
    """Tests for FastAPIApp decorator methods without native bindings."""

    def setup_method(self):
        """Disable native bindings for decorator tests."""
        import fasterapi.fastapi_compat as compat
        self.original_native = compat.HAS_NATIVE
        compat.HAS_NATIVE = False

    def teardown_method(self):
        """Restore native bindings state."""
        import fasterapi.fastapi_compat as compat
        compat.HAS_NATIVE = self.original_native

    def test_get_decorator_returns_function(self):
        """Test @app.get returns the original function."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.get("/users")
        def get_users():
            return []

        # In fallback mode, function should be unchanged
        assert callable(get_users)
        assert get_users() == []

    def test_post_decorator_returns_function(self):
        """Test @app.post returns the original function."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.post("/users")
        def create_user(name: str):
            return {"name": name}

        assert callable(create_user)
        assert create_user("Alice") == {"name": "Alice"}

    def test_put_decorator_returns_function(self):
        """Test @app.put returns the original function."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.put("/users/{user_id}")
        def update_user(user_id: int, name: str):
            return {"id": user_id, "name": name}

        assert update_user(1, "Bob") == {"id": 1, "name": "Bob"}

    def test_delete_decorator_returns_function(self):
        """Test @app.delete returns the original function."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.delete("/users/{user_id}")
        def delete_user(user_id: int):
            return {"deleted": user_id}

        assert delete_user(42) == {"deleted": 42}

    def test_patch_decorator_returns_function(self):
        """Test @app.patch returns the original function."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.patch("/users/{user_id}")
        def patch_user(user_id: int, name: str = None):
            return {"id": user_id, "name": name}

        assert patch_user(1, "Charlie") == {"id": 1, "name": "Charlie"}

    def test_decorator_with_response_model(self):
        """Test decorator with response_model parameter."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.get("/users/{user_id}", response_model=dict)
        def get_user(user_id: int):
            return {"id": user_id}

        assert get_user(1) == {"id": 1}

    def test_decorator_with_tags(self):
        """Test decorator with tags parameter."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.get("/users", tags=["users", "public"])
        def get_users():
            return []

        assert get_users() == []

    def test_decorator_with_summary_description(self):
        """Test decorator with summary and description."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.get(
            "/health",
            summary="Health check",
            description="Check API health status"
        )
        def health_check():
            return {"status": "ok"}

        assert health_check() == {"status": "ok"}

    def test_multiple_routes_same_app(self):
        """Test registering multiple routes on same app."""
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.get("/")
        def root():
            return {"message": "root"}

        @app.get("/users")
        def get_users():
            return []

        @app.post("/users")
        def create_user(name: str):
            return {"name": name}

        @app.get("/users/{user_id}")
        def get_user(user_id: int):
            return {"id": user_id}

        # All functions should work
        assert root() == {"message": "root"}
        assert get_users() == []
        assert create_user("Test") == {"name": "Test"}
        assert get_user(123) == {"id": 123}

    def test_async_handler(self):
        """Test async handler function."""
        import asyncio
        from fasterapi.fastapi_compat import FastAPIApp
        app = FastAPIApp()

        @app.get("/async")
        async def async_handler():
            return {"async": True}

        # Should return coroutine
        result = async_handler()
        assert asyncio.iscoroutine(result)
        assert asyncio.run(result) == {"async": True}


# =============================================================================
# Route Decorator Factory Tests
# =============================================================================

class TestRouteDecorator:
    """Tests for route_decorator function."""

    def setup_method(self):
        """Disable native bindings for tests."""
        import fasterapi.fastapi_compat as compat
        self.original_native = compat.HAS_NATIVE
        compat.HAS_NATIVE = False

    def teardown_method(self):
        """Restore native bindings state."""
        import fasterapi.fastapi_compat as compat
        compat.HAS_NATIVE = self.original_native

    def test_route_decorator_preserves_function(self):
        """Test route_decorator preserves function in fallback mode."""
        from fasterapi.fastapi_compat import route_decorator

        def handler(user_id: int):
            return {"id": user_id}

        decorated = route_decorator('GET', '/users/{user_id}')(handler)
        assert decorated is handler

    def test_route_decorator_preserves_async_function(self):
        """Test route_decorator preserves async function."""
        from fasterapi.fastapi_compat import route_decorator
        import asyncio

        async def async_handler(user_id: int):
            return {"id": user_id}

        decorated = route_decorator('POST', '/users/{user_id}')(async_handler)
        assert asyncio.iscoroutinefunction(decorated)


# =============================================================================
# FastAPI Alias Test
# =============================================================================

class TestFastAPIAlias:
    """Test FastAPI alias for compatibility."""

    def test_fastapi_alias_exists(self):
        """Test that FastAPI alias points to FastAPIApp."""
        from fasterapi.fastapi_compat import FastAPI, FastAPIApp
        assert FastAPI is FastAPIApp

    def test_fastapi_import_style(self):
        """Test typical FastAPI import pattern works."""
        import fasterapi.fastapi_compat as compat
        original_native = compat.HAS_NATIVE
        compat.HAS_NATIVE = False

        try:
            from fasterapi.fastapi_compat import FastAPI
            app = FastAPI(title="Test API")
            assert app.title == "Test API"
        finally:
            compat.HAS_NATIVE = original_native


# =============================================================================
# Edge Cases and Error Handling
# =============================================================================

class TestEdgeCases:
    """Edge case tests for FastAPI shim."""

    def test_empty_path_pattern(self):
        """Test with empty path parameter."""
        def handler():
            pass

        params = extract_function_parameters(handler, '', 'GET')
        assert params == []

    def test_root_path(self):
        """Test with root path."""
        def handler():
            pass

        params = extract_function_parameters(handler, '/', 'GET')
        assert params == []

    def test_path_with_no_params_but_query(self):
        """Test path without params but with query params."""
        def search(q: str, limit: int = 10):
            pass

        params = extract_function_parameters(search, '/search', 'GET')
        assert len(params) == 2
        for p in params:
            assert p['location'] == 'query'

    def test_parameter_named_path(self):
        """Test parameter literally named 'path'."""
        def handler(path: str):
            pass

        # Not in path pattern, so should be query
        params = extract_function_parameters(handler, '/handler', 'GET')
        assert params[0]['location'] == 'query'

        # In path pattern
        params = extract_function_parameters(handler, '/handler/{path}', 'GET')
        assert params[0]['location'] == 'path'

    def test_underscore_param_name(self):
        """Test parameter with underscores."""
        def handler(user_id: int, item_name: str):
            pass

        params = extract_function_parameters(
            handler, '/users/{user_id}/items/{item_name}', 'GET'
        )
        assert len(params) == 2
        param_names = {p['name'] for p in params}
        assert param_names == {'user_id', 'item_name'}

    def test_numeric_param_in_path(self):
        """Test path parameter with numbers in name."""
        def handler(id123: int):
            pass

        params = extract_function_parameters(handler, '/items/{id123}', 'GET')
        assert params[0]['name'] == 'id123'
        assert params[0]['location'] == 'path'

    def test_very_long_function_signature(self):
        """Test function with many parameters."""
        def handler(
            a: str, b: int, c: float, d: bool,
            e: str = 'e', f: int = 1, g: float = 1.0, h: bool = True
        ):
            pass

        params = extract_function_parameters(
            handler, '/items/{a}/{b}/{c}/{d}', 'POST'
        )
        assert len(params) == 8

        path_params = [p for p in params if p['location'] == 'path']
        query_params = [p for p in params if p['location'] == 'query']
        assert len(path_params) == 4
        assert len(query_params) == 4

    def test_none_default_value(self):
        """Test parameter with None as explicit default."""
        def handler(optional_param: str = None):
            pass

        params = extract_function_parameters(handler, '/handler', 'GET')
        assert params[0]['required'] is False
        assert params[0]['default'] == 'None'

    @requires_pydantic
    def test_multiple_pydantic_models_only_first_is_body(self):
        """Test that only first Pydantic model is body (typical pattern)."""
        class RequestBody(BaseModel):
            data: str

        class ResponseModel(BaseModel):
            result: str

        # In practice, return type annotation isn't a parameter
        def handler(body: RequestBody):
            pass

        params = extract_function_parameters(handler, '/handler', 'POST')
        assert len(params) == 1
        assert params[0]['location'] == 'body'


# =============================================================================
# Integration-Style Tests (Still Unit Level)
# =============================================================================

class TestFastAPIStyleUsage:
    """Tests that verify typical FastAPI usage patterns work."""

    def setup_method(self):
        """Disable native bindings."""
        import fasterapi.fastapi_compat as compat
        self.original_native = compat.HAS_NATIVE
        compat.HAS_NATIVE = False

    def teardown_method(self):
        """Restore native bindings."""
        import fasterapi.fastapi_compat as compat
        compat.HAS_NATIVE = self.original_native

    def test_typical_crud_app(self):
        """Test typical CRUD application pattern."""
        from fasterapi.fastapi_compat import FastAPI

        app = FastAPI(title="User API", version="1.0.0")

        users_db = {}

        @app.get("/users")
        def list_users():
            return list(users_db.values())

        @app.get("/users/{user_id}")
        def get_user(user_id: int):
            return users_db.get(user_id)

        @app.post("/users")
        def create_user(name: str, email: str):
            user_id = len(users_db) + 1
            user = {"id": user_id, "name": name, "email": email}
            users_db[user_id] = user
            return user

        @app.put("/users/{user_id}")
        def update_user(user_id: int, name: str, email: str):
            if user_id in users_db:
                users_db[user_id]["name"] = name
                users_db[user_id]["email"] = email
            return users_db.get(user_id)

        @app.delete("/users/{user_id}")
        def delete_user(user_id: int):
            return users_db.pop(user_id, None)

        # Test the handlers work
        assert list_users() == []
        user = create_user("Alice", "alice@example.com")
        assert user["name"] == "Alice"
        assert len(list_users()) == 1
        assert get_user(1)["name"] == "Alice"
        update_user(1, "Alice Updated", "alice@new.com")
        assert get_user(1)["name"] == "Alice Updated"
        delete_user(1)
        assert list_users() == []

    @requires_pydantic
    def test_pydantic_request_body_pattern(self):
        """Test typical Pydantic request body pattern."""
        from fasterapi.fastapi_compat import FastAPI

        class Item(BaseModel):
            name: str
            price: float
            description: Optional[str] = None

        app = FastAPI()
        items = []

        @app.post("/items")
        def create_item(item: Item):
            items.append(item)
            return item

        @app.get("/items")
        def list_items():
            return items

        # Test with Pydantic model
        item = Item(name="Widget", price=9.99)
        result = create_item(item)
        assert result.name == "Widget"
        assert len(list_items()) == 1

    def test_nested_routes_pattern(self):
        """Test nested route patterns."""
        from fasterapi.fastapi_compat import FastAPI

        app = FastAPI()
        data = {
            "org1": {
                "project1": {"tasks": ["task1", "task2"]},
                "project2": {"tasks": ["task3"]},
            }
        }

        @app.get("/orgs/{org_id}/projects/{project_id}/tasks")
        def get_tasks(org_id: str, project_id: str):
            return data.get(org_id, {}).get(project_id, {}).get("tasks", [])

        @app.get("/orgs/{org_id}/projects/{project_id}/tasks/{task_id}")
        def get_task(org_id: str, project_id: str, task_id: int):
            tasks = data.get(org_id, {}).get(project_id, {}).get("tasks", [])
            return tasks[task_id] if 0 <= task_id < len(tasks) else None

        assert get_tasks("org1", "project1") == ["task1", "task2"]
        assert get_task("org1", "project1", 0) == "task1"

    def test_query_param_filtering_pattern(self):
        """Test query parameter filtering pattern."""
        from fasterapi.fastapi_compat import FastAPI

        app = FastAPI()
        items = [
            {"id": 1, "name": "Apple", "category": "fruit"},
            {"id": 2, "name": "Banana", "category": "fruit"},
            {"id": 3, "name": "Carrot", "category": "vegetable"},
        ]

        @app.get("/items")
        def list_items(category: str = None, limit: int = 10, offset: int = 0):
            filtered = items
            if category:
                filtered = [i for i in filtered if i["category"] == category]
            return filtered[offset:offset + limit]

        assert len(list_items()) == 3
        assert len(list_items(category="fruit")) == 2
        assert len(list_items(limit=1)) == 1
        assert list_items(offset=1)[0]["name"] == "Banana"


# =============================================================================
# Randomized Stress Tests
# =============================================================================

class TestRandomizedStress:
    """Stress tests with randomized data."""

    def test_random_path_patterns(self):
        """Test random path pattern generation."""
        for _ in range(50):
            num_segments = random.randint(1, 5)
            segments = []
            param_names = []

            for i in range(num_segments):
                if random.choice([True, False]):
                    # Static segment
                    segments.append(random_string(6))
                else:
                    # Path parameter
                    param = random_string(8)
                    param_names.append(param)
                    segments.append(f'{{{param}}}')

            path = '/' + '/'.join(segments)

            # Create function with matching params
            params_str = ', '.join(f'{p}: str' for p in param_names)
            func_code = f"def handler({params_str}): pass"
            local_ns = {}
            exec(func_code, {}, local_ns)
            handler = local_ns['handler']

            # Extract and verify
            extracted = extract_function_parameters(handler, path, 'GET')
            for param in extracted:
                if param['name'] in param_names:
                    assert param['location'] == 'path'

    def test_random_query_defaults(self):
        """Test random default values."""
        for _ in range(30):
            default_int = random_int()
            default_str = random_string(10)
            default_float = random.uniform(0, 100)

            def handler(
                i: int = default_int,
                s: str = default_str,
                f: float = default_float
            ):
                pass

            params = extract_function_parameters(handler, '/test', 'GET')
            assert len(params) == 3

            for p in params:
                assert p['required'] is False
                if p['name'] == 'i':
                    assert p['default'] == str(default_int)
                elif p['name'] == 's':
                    assert p['default'] == default_str
                elif p['name'] == 'f':
                    assert p['default'] == str(default_float)


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
