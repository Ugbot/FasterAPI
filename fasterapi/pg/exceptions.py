"""PostgreSQL exception hierarchy."""


class PgError(Exception):
    """Base exception for all PostgreSQL operations."""
    
    def __init__(self, message: str, code: int | None = None, detail: str | None = None):
        self.message = message
        self.code = code
        self.detail = detail
        super().__init__(message)


class PgConnectionError(PgError):
    """Connection-related errors (pool exhausted, connection lost, etc.)."""
    pass


class PgTimeout(PgError):
    """Query or operation exceeded deadline."""
    pass


class PgCanceled(PgError):
    """Query was canceled by client or server."""
    pass


class PgIntegrityError(PgError):
    """Integrity constraint violation."""
    pass


class PgDataError(PgError):
    """Invalid data for column type."""
    pass
