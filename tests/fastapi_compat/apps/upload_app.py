"""
File upload application for FastAPI compatibility testing.

Tests:
- Form data handling
- Single file upload
- Multiple file uploads
- Mixed form data and files
- File validation
- UploadFile attributes

Can be run with either FastAPI or FasterAPI by setting TEST_FRAMEWORK env var.
"""

import hashlib
import os
from datetime import datetime
from typing import Dict, List, Optional
from uuid import uuid4

# Import framework based on environment
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi import FastAPI, File, Form, HTTPException, UploadFile
    from fastapi.responses import JSONResponse
    from pydantic import BaseModel
else:
    from pydantic import BaseModel

    from fasterapi import FastAPI, File, Form, HTTPException, UploadFile
    from fasterapi.responses import JSONResponse


# Pydantic Models
class FileInfo(BaseModel):
    id: str
    filename: str
    content_type: str
    size: int
    md5_hash: str
    uploaded_at: datetime


class FormDataResponse(BaseModel):
    username: str
    email: str
    age: Optional[int] = None
    bio: Optional[str] = None


# In-memory file storage
uploaded_files: Dict[str, dict] = {}


def create_app() -> FastAPI:
    """Create and configure the FastAPI application."""

    app = FastAPI(
        title="File Upload Test App",
        description="File upload testing application",
        version="1.0.0",
    )

    # Form data only
    @app.post("/form/simple", response_model=FormDataResponse, tags=["form"])
    async def simple_form(
        username: str = Form(..., min_length=3),
        email: str = Form(...),
    ):
        """Handle simple form data."""
        return {"username": username, "email": email}

    @app.post("/form/complex", response_model=FormDataResponse, tags=["form"])
    async def complex_form(
        username: str = Form(..., min_length=3, max_length=50),
        email: str = Form(...),
        age: Optional[int] = Form(None, ge=0, le=150),
        bio: Optional[str] = Form(None, max_length=500),
    ):
        """Handle complex form data with optional fields."""
        return {
            "username": username,
            "email": email,
            "age": age,
            "bio": bio,
        }

    # Single file upload
    @app.post("/upload/single", response_model=FileInfo, tags=["upload"])
    async def upload_single_file(
        file: UploadFile = File(...),
    ):
        """Upload a single file."""
        content = await file.read()
        file_id = str(uuid4())

        # Calculate MD5 hash
        md5_hash = hashlib.md5(content).hexdigest()

        file_info = {
            "id": file_id,
            "filename": file.filename,
            "content_type": file.content_type or "application/octet-stream",
            "size": len(content),
            "md5_hash": md5_hash,
            "uploaded_at": datetime.utcnow(),
            "content": content,  # Store for testing
        }

        uploaded_files[file_id] = file_info

        # Return without content
        return {k: v for k, v in file_info.items() if k != "content"}

    @app.post("/upload/optional", tags=["upload"])
    async def upload_optional_file(
        file: Optional[UploadFile] = File(None),
    ):
        """Upload an optional file."""
        if file is None:
            return {"message": "No file uploaded", "has_file": False}

        content = await file.read()
        return {
            "message": "File uploaded",
            "has_file": True,
            "filename": file.filename,
            "size": len(content),
        }

    # Multiple file upload
    @app.post("/upload/multiple", tags=["upload"])
    async def upload_multiple_files(
        files: List[UploadFile] = File(...),
    ):
        """Upload multiple files."""
        results = []

        for file in files:
            content = await file.read()
            file_id = str(uuid4())
            md5_hash = hashlib.md5(content).hexdigest()

            file_info = {
                "id": file_id,
                "filename": file.filename,
                "content_type": file.content_type or "application/octet-stream",
                "size": len(content),
                "md5_hash": md5_hash,
            }
            results.append(file_info)

        return {
            "count": len(results),
            "files": results,
        }

    # Mixed form data and files
    @app.post("/upload/with-form", tags=["upload"])
    async def upload_with_form(
        title: str = Form(...),
        description: Optional[str] = Form(None),
        file: UploadFile = File(...),
    ):
        """Upload a file with form data."""
        content = await file.read()

        return {
            "title": title,
            "description": description,
            "file": {
                "filename": file.filename,
                "content_type": file.content_type,
                "size": len(content),
            },
        }

    @app.post("/upload/multiple-with-form", tags=["upload"])
    async def upload_multiple_with_form(
        title: str = Form(...),
        tags: List[str] = Form([]),
        files: List[UploadFile] = File(...),
    ):
        """Upload multiple files with form data."""
        file_infos = []
        for file in files:
            content = await file.read()
            file_infos.append(
                {
                    "filename": file.filename,
                    "size": len(content),
                }
            )

        return {
            "title": title,
            "tags": tags,
            "files": file_infos,
        }

    # File validation
    @app.post("/upload/image", tags=["validation"])
    async def upload_image(
        file: UploadFile = File(...),
    ):
        """Upload an image file (validates content type)."""
        allowed_types = ["image/jpeg", "image/png", "image/gif", "image/webp"]

        if file.content_type not in allowed_types:
            raise HTTPException(
                status_code=400,
                detail=f"Invalid file type. Allowed: {', '.join(allowed_types)}",
            )

        content = await file.read()

        return {
            "filename": file.filename,
            "content_type": file.content_type,
            "size": len(content),
        }

    @app.post("/upload/size-limit", tags=["validation"])
    async def upload_with_size_limit(
        file: UploadFile = File(...),
        max_size: int = Form(1024 * 1024),  # Default 1MB
    ):
        """Upload a file with size limit."""
        content = await file.read()

        if len(content) > max_size:
            raise HTTPException(
                status_code=400,
                detail=f"File too large. Max size: {max_size} bytes",
            )

        return {
            "filename": file.filename,
            "size": len(content),
            "max_allowed": max_size,
        }

    # File info endpoint
    @app.get("/files/{file_id}", response_model=FileInfo, tags=["files"])
    async def get_file_info(file_id: str):
        """Get info about an uploaded file."""
        if file_id not in uploaded_files:
            raise HTTPException(status_code=404, detail="File not found")

        file_info = uploaded_files[file_id]
        return {k: v for k, v in file_info.items() if k != "content"}

    @app.get("/files/{file_id}/download", tags=["files"])
    async def download_file(file_id: str):
        """Download an uploaded file."""
        if file_id not in uploaded_files:
            raise HTTPException(status_code=404, detail="File not found")

        file_info = uploaded_files[file_id]

        return JSONResponse(
            content={
                "filename": file_info["filename"],
                "content_base64": file_info["content"].hex(),
                "size": file_info["size"],
            }
        )

    # UploadFile attributes test
    @app.post("/upload/attributes", tags=["upload"])
    async def test_upload_attributes(
        file: UploadFile = File(...),
    ):
        """Test UploadFile attributes."""
        # Read first chunk
        first_chunk = await file.read(100)

        # Seek back
        await file.seek(0)

        # Read all
        content = await file.read()

        return {
            "filename": file.filename,
            "content_type": file.content_type,
            "size": len(content),
            "first_chunk_size": len(first_chunk),
            "headers": dict(file.headers)
            if hasattr(file, "headers") and file.headers
            else {},
        }

    @app.get("/health", tags=["system"])
    async def health():
        """Health check."""
        return {"status": "healthy", "framework": FRAMEWORK}

    return app


# Create app instance
app = create_app()


def clear_files():
    """Clear all uploaded files (for testing)."""
    uploaded_files.clear()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
