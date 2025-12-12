"""
OAuth2 authentication utilities for FasterAPI.

Provides OAuth2PasswordBearer and related classes.
"""

from typing import Any, Dict, List, Optional

from fasterapi.exceptions import HTTPException


class OAuth2:
    """
    Base OAuth2 scheme.

    This is a base class for OAuth2 authentication flows.
    """

    def __init__(
        self,
        *,
        flows: Dict[str, Any] = None,
        scheme_name: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ):
        """
        Initialize OAuth2 scheme.

        Args:
            flows: OAuth2 flows configuration for OpenAPI
            scheme_name: Name for OpenAPI schema
            description: Description for OpenAPI
            auto_error: If True, raise HTTPException on auth failure
        """
        self.flows = flows or {}
        self.scheme_name = scheme_name or self.__class__.__name__
        self.description = description
        self.auto_error = auto_error


class OAuth2PasswordBearer(OAuth2):
    """
    OAuth2 password flow with Bearer token.

    Extracts and validates Bearer tokens from the Authorization header.

    Usage:
        oauth2_scheme = OAuth2PasswordBearer(tokenUrl="token")

        @app.get("/users/me")
        def get_current_user(token: str = Depends(oauth2_scheme)):
            user = decode_token(token)
            return user
    """

    def __init__(
        self,
        tokenUrl: str,
        scheme_name: Optional[str] = None,
        scopes: Optional[Dict[str, str]] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ):
        """
        Initialize OAuth2PasswordBearer.

        Args:
            tokenUrl: URL to obtain the token
            scheme_name: Name for OpenAPI schema
            scopes: Available OAuth2 scopes
            description: Description for OpenAPI
            auto_error: If True, raise HTTPException on auth failure
        """
        flows = {
            "password": {
                "tokenUrl": tokenUrl,
                "scopes": scopes or {},
            }
        }
        super().__init__(
            flows=flows,
            scheme_name=scheme_name,
            description=description,
            auto_error=auto_error,
        )
        self.tokenUrl = tokenUrl
        self.scopes = scopes or {}

    async def __call__(self, request: Any) -> Optional[str]:
        """
        Extract Bearer token from request.

        Args:
            request: HTTP request object

        Returns:
            The bearer token, or None if auto_error is False

        Raises:
            HTTPException: If authentication fails and auto_error is True
        """
        # Get Authorization header
        authorization = None
        if hasattr(request, "get_header"):
            authorization = request.get_header("authorization")
        elif hasattr(request, "headers"):
            authorization = request.headers.get("authorization")

        if not authorization:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Not authenticated",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        # Parse Bearer token
        scheme, _, token = authorization.partition(" ")
        if scheme.lower() != "bearer":
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Invalid authentication scheme",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        if not token:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Invalid token",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        return token


class OAuth2AuthorizationCodeBearer(OAuth2):
    """
    OAuth2 authorization code flow with Bearer token.

    Usage:
        oauth2_scheme = OAuth2AuthorizationCodeBearer(
            authorizationUrl="https://example.com/oauth/authorize",
            tokenUrl="https://example.com/oauth/token",
        )
    """

    def __init__(
        self,
        authorizationUrl: str,
        tokenUrl: str,
        refreshUrl: Optional[str] = None,
        scheme_name: Optional[str] = None,
        scopes: Optional[Dict[str, str]] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ):
        """
        Initialize OAuth2AuthorizationCodeBearer.

        Args:
            authorizationUrl: URL for authorization
            tokenUrl: URL to obtain the token
            refreshUrl: URL to refresh the token
            scheme_name: Name for OpenAPI schema
            scopes: Available OAuth2 scopes
            description: Description for OpenAPI
            auto_error: If True, raise HTTPException on auth failure
        """
        flows = {
            "authorizationCode": {
                "authorizationUrl": authorizationUrl,
                "tokenUrl": tokenUrl,
                "scopes": scopes or {},
            }
        }
        if refreshUrl:
            flows["authorizationCode"]["refreshUrl"] = refreshUrl

        super().__init__(
            flows=flows,
            scheme_name=scheme_name,
            description=description,
            auto_error=auto_error,
        )
        self.authorizationUrl = authorizationUrl
        self.tokenUrl = tokenUrl
        self.refreshUrl = refreshUrl
        self.scopes = scopes or {}

    async def __call__(self, request: Any) -> Optional[str]:
        """Extract Bearer token from request."""
        # Same logic as OAuth2PasswordBearer
        authorization = None
        if hasattr(request, "get_header"):
            authorization = request.get_header("authorization")
        elif hasattr(request, "headers"):
            authorization = request.headers.get("authorization")

        if not authorization:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Not authenticated",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        scheme, _, token = authorization.partition(" ")
        if scheme.lower() != "bearer" or not token:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Invalid authentication credentials",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        return token


class OAuth2PasswordRequestForm:
    """
    OAuth2 password flow request form.

    Parses the standard OAuth2 password grant request body.

    Usage:
        @app.post("/token")
        def login(form_data: OAuth2PasswordRequestForm = Depends()):
            user = authenticate(form_data.username, form_data.password)
            return {"access_token": create_token(user), "token_type": "bearer"}
    """

    def __init__(
        self,
        *,
        grant_type: Optional[str] = None,
        username: str = "",
        password: str = "",
        scope: str = "",
        client_id: Optional[str] = None,
        client_secret: Optional[str] = None,
    ):
        """
        Initialize OAuth2PasswordRequestForm.

        This is typically created by the framework from form data.

        Args:
            grant_type: Grant type (should be "password")
            username: Username
            password: Password
            scope: Space-separated scopes
            client_id: Client ID
            client_secret: Client secret
        """
        self.grant_type = grant_type
        self.username = username
        self.password = password
        self.scopes = scope.split() if scope else []
        self.client_id = client_id
        self.client_secret = client_secret

    @classmethod
    async def from_request(cls, request: Any) -> "OAuth2PasswordRequestForm":
        """
        Create from form data in request.

        Args:
            request: HTTP request with form data

        Returns:
            OAuth2PasswordRequestForm instance
        """
        # Get form data
        if hasattr(request, "form"):
            form_data = await request.form()
        else:
            form_data = {}

        return cls(
            grant_type=form_data.get("grant_type"),
            username=form_data.get("username", ""),
            password=form_data.get("password", ""),
            scope=form_data.get("scope", ""),
            client_id=form_data.get("client_id"),
            client_secret=form_data.get("client_secret"),
        )


class OAuth2PasswordRequestFormStrict(OAuth2PasswordRequestForm):
    """
    Strict OAuth2 password flow request form.

    Same as OAuth2PasswordRequestForm but requires grant_type to be "password".
    """

    def __init__(
        self,
        *,
        grant_type: str = "password",
        username: str = "",
        password: str = "",
        scope: str = "",
        client_id: Optional[str] = None,
        client_secret: Optional[str] = None,
    ):
        if grant_type != "password":
            raise ValueError(f"grant_type must be 'password', got '{grant_type}'")
        super().__init__(
            grant_type=grant_type,
            username=username,
            password=password,
            scope=scope,
            client_id=client_id,
            client_secret=client_secret,
        )


class SecurityScopes:
    """
    Security scopes for OAuth2.

    Provides information about required scopes for a route.

    Usage:
        @app.get("/items")
        def get_items(
            token: str = Security(oauth2_scheme, scopes=["items:read"]),
            security_scopes: SecurityScopes = Depends(),
        ):
            verify_scopes(token, security_scopes.scopes)
            return items
    """

    def __init__(
        self,
        scopes: Optional[List[str]] = None,
    ):
        """
        Initialize SecurityScopes.

        Args:
            scopes: List of required scopes
        """
        self.scopes = scopes or []
        self.scope_str = " ".join(self.scopes)
