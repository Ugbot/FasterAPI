"""
Authentication application for FastAPI compatibility testing.

Tests:
- OAuth2 Password Bearer
- HTTP Basic authentication
- API Key (header, query, cookie)
- Dependency injection for auth
- Security scopes

Can be run with either FastAPI or FasterAPI by setting TEST_FRAMEWORK env var.
"""

import os
import secrets
from datetime import datetime, timedelta
from typing import Dict, List, Optional
from uuid import uuid4

# Import framework based on environment
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi import Depends, FastAPI, HTTPException, Security, status
    from fastapi.security import (
        APIKeyCookie,
        APIKeyHeader,
        APIKeyQuery,
        HTTPBasic,
        HTTPBasicCredentials,
        OAuth2PasswordBearer,
        OAuth2PasswordRequestForm,
    )
    from pydantic import BaseModel
else:
    from pydantic import BaseModel

    from fasterapi import Depends, FastAPI, HTTPException, Security, status
    from fasterapi.security import (
        APIKeyCookie,
        APIKeyHeader,
        APIKeyQuery,
        HTTPBasic,
        HTTPBasicCredentials,
        OAuth2PasswordBearer,
        OAuth2PasswordRequestForm,
    )


# Pydantic Models
class Token(BaseModel):
    access_token: str
    token_type: str


class TokenData(BaseModel):
    username: Optional[str] = None
    scopes: List[str] = []


class User(BaseModel):
    username: str
    email: Optional[str] = None
    full_name: Optional[str] = None
    disabled: bool = False
    scopes: List[str] = []


class UserInDB(User):
    hashed_password: str


# Fake database
fake_users_db: Dict[str, dict] = {
    "testuser": {
        "username": "testuser",
        "email": "test@example.com",
        "full_name": "Test User",
        "disabled": False,
        "hashed_password": "fakehashed_password123",
        "scopes": ["items:read", "items:write"],
    },
    "admin": {
        "username": "admin",
        "email": "admin@example.com",
        "full_name": "Admin User",
        "disabled": False,
        "hashed_password": "fakehashed_adminpass",
        "scopes": ["items:read", "items:write", "users:read", "users:write", "admin"],
    },
    "readonly": {
        "username": "readonly",
        "email": "readonly@example.com",
        "full_name": "Read Only User",
        "disabled": False,
        "hashed_password": "fakehashed_readonly",
        "scopes": ["items:read"],
    },
    "disabled": {
        "username": "disabled",
        "email": "disabled@example.com",
        "full_name": "Disabled User",
        "disabled": True,
        "hashed_password": "fakehashed_disabled",
        "scopes": [],
    },
}

# Fake token storage
active_tokens: Dict[str, dict] = {}

# API Keys
valid_api_keys: Dict[str, str] = {
    "key-header-123": "testuser",
    "key-query-456": "testuser",
    "key-cookie-789": "admin",
}


def fake_hash_password(password: str) -> str:
    return f"fakehashed_{password}"


def verify_password(plain_password: str, hashed_password: str) -> bool:
    return fake_hash_password(plain_password) == hashed_password


def get_user(db: Dict, username: str) -> Optional[UserInDB]:
    if username in db:
        user_dict = db[username]
        return UserInDB(**user_dict)
    return None


def authenticate_user(db: Dict, username: str, password: str) -> Optional[UserInDB]:
    user = get_user(db, username)
    if not user:
        return None
    if not verify_password(password, user.hashed_password):
        return None
    return user


def create_access_token(
    username: str, scopes: List[str], expires_delta: timedelta = None
) -> str:
    token = secrets.token_urlsafe(32)
    expire = datetime.utcnow() + (expires_delta or timedelta(minutes=30))
    active_tokens[token] = {
        "username": username,
        "scopes": scopes,
        "expires": expire,
    }
    return token


def create_app() -> FastAPI:
    """Create and configure the FastAPI application."""

    app = FastAPI(
        title="Auth Test App",
        description="Authentication testing application",
        version="1.0.0",
    )

    # Security schemes
    oauth2_scheme = OAuth2PasswordBearer(
        tokenUrl="token",
        scopes={
            "items:read": "Read items",
            "items:write": "Write items",
            "users:read": "Read users",
            "users:write": "Write users",
            "admin": "Admin access",
        },
    )

    http_basic = HTTPBasic()
    api_key_header = APIKeyHeader(name="X-API-Key", auto_error=False)
    api_key_query = APIKeyQuery(name="api_key", auto_error=False)
    api_key_cookie = APIKeyCookie(name="api_key", auto_error=False)

    # Dependency functions
    async def get_current_user(token: str = Depends(oauth2_scheme)) -> User:
        """Get user from OAuth2 token."""
        if token not in active_tokens:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Invalid authentication credentials",
                headers={"WWW-Authenticate": "Bearer"},
            )

        token_data = active_tokens[token]
        if datetime.utcnow() > token_data["expires"]:
            del active_tokens[token]
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Token expired",
                headers={"WWW-Authenticate": "Bearer"},
            )

        user = get_user(fake_users_db, token_data["username"])
        if user is None:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="User not found",
                headers={"WWW-Authenticate": "Bearer"},
            )

        return User(**user.model_dump())

    async def get_current_active_user(
        current_user: User = Depends(get_current_user),
    ) -> User:
        """Get active user (not disabled)."""
        if current_user.disabled:
            raise HTTPException(status_code=400, detail="Inactive user")
        return current_user

    async def get_basic_auth_user(
        credentials: HTTPBasicCredentials = Depends(http_basic),
    ) -> User:
        """Get user from HTTP Basic auth."""
        user = authenticate_user(
            fake_users_db, credentials.username, credentials.password
        )
        if not user:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Incorrect username or password",
                headers={"WWW-Authenticate": "Basic"},
            )
        return User(**user.model_dump())

    async def get_api_key_user(
        api_key_header: str = Security(api_key_header),
        api_key_query: str = Security(api_key_query),
        api_key_cookie: str = Security(api_key_cookie),
    ) -> User:
        """Get user from API key (header, query, or cookie)."""
        api_key = api_key_header or api_key_query or api_key_cookie
        if not api_key:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail="Not authenticated",
            )

        if api_key not in valid_api_keys:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail="Invalid API key",
            )

        username = valid_api_keys[api_key]
        user = get_user(fake_users_db, username)
        if not user:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail="User not found",
            )

        return User(**user.model_dump())

    def require_scopes(*required_scopes: str):
        """Dependency factory for scope checking."""

        async def check_scopes(
            current_user: User = Depends(get_current_active_user),
        ) -> User:
            for scope in required_scopes:
                if scope not in current_user.scopes:
                    raise HTTPException(
                        status_code=status.HTTP_403_FORBIDDEN,
                        detail=f"Missing required scope: {scope}",
                    )
            return current_user

        return check_scopes

    # OAuth2 endpoints
    @app.post("/token", response_model=Token, tags=["auth"])
    async def login(form_data: OAuth2PasswordRequestForm = Depends()):
        """OAuth2 token endpoint."""
        user = authenticate_user(fake_users_db, form_data.username, form_data.password)
        if not user:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="Incorrect username or password",
                headers={"WWW-Authenticate": "Bearer"},
            )

        # Filter scopes to only those the user has
        granted_scopes = [s for s in form_data.scopes if s in user.scopes]
        if not granted_scopes:
            granted_scopes = user.scopes

        access_token = create_access_token(
            username=user.username,
            scopes=granted_scopes,
            expires_delta=timedelta(minutes=30),
        )

        return {"access_token": access_token, "token_type": "bearer"}

    @app.get("/users/me", response_model=User, tags=["oauth2"])
    async def read_users_me(current_user: User = Depends(get_current_active_user)):
        """Get current user info via OAuth2."""
        return current_user

    @app.get("/users/me/items", tags=["oauth2"])
    async def read_own_items(
        current_user: User = Depends(require_scopes("items:read")),
    ):
        """Get current user's items (requires items:read scope)."""
        return {"items": [{"id": 1, "name": "Item 1"}, {"id": 2, "name": "Item 2"}]}

    @app.post("/users/me/items", tags=["oauth2"])
    async def create_own_item(
        item_name: str,
        current_user: User = Depends(require_scopes("items:write")),
    ):
        """Create item for current user (requires items:write scope)."""
        return {"item": {"id": 3, "name": item_name, "owner": current_user.username}}

    @app.get("/admin/users", tags=["oauth2"])
    async def admin_list_users(current_user: User = Depends(require_scopes("admin"))):
        """List all users (requires admin scope)."""
        return {"users": list(fake_users_db.keys())}

    # HTTP Basic endpoints
    @app.get("/basic/me", response_model=User, tags=["basic"])
    async def basic_read_me(user: User = Depends(get_basic_auth_user)):
        """Get current user via HTTP Basic auth."""
        return user

    @app.get("/basic/items", tags=["basic"])
    async def basic_read_items(user: User = Depends(get_basic_auth_user)):
        """Get items via HTTP Basic auth."""
        return {"items": ["item1", "item2"], "user": user.username}

    # API Key endpoints
    @app.get("/apikey/me", response_model=User, tags=["apikey"])
    async def apikey_read_me(user: User = Depends(get_api_key_user)):
        """Get current user via API key."""
        return user

    @app.get("/apikey/items", tags=["apikey"])
    async def apikey_read_items(user: User = Depends(get_api_key_user)):
        """Get items via API key."""
        return {"items": ["item1", "item2"], "user": user.username}

    # Public endpoints
    @app.get("/public", tags=["public"])
    async def public_endpoint():
        """Public endpoint - no auth required."""
        return {"message": "This is public"}

    @app.get("/health", tags=["public"])
    async def health():
        """Health check."""
        return {"status": "healthy", "framework": FRAMEWORK}

    return app


# Create app instance
app = create_app()


def clear_tokens():
    """Clear all active tokens (for testing)."""
    active_tokens.clear()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
