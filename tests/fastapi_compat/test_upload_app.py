"""
Tests for file upload app compatibility between FastAPI and FasterAPI.

Run with:
    TEST_FRAMEWORK=fastapi pytest tests/fastapi_compat/test_upload_app.py -v
    TEST_FRAMEWORK=fasterapi pytest tests/fastapi_compat/test_upload_app.py -v
"""

import hashlib
import io
import os
import random
import string

import pytest
from apps.upload_app import app, clear_files, uploaded_files

FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi.testclient import TestClient
else:
    try:
        from fasterapi.testclient import TestClient
    except ImportError:
        from starlette.testclient import TestClient


@pytest.fixture(autouse=True)
def clean_files():
    """Clear files before each test."""
    clear_files()
    yield
    clear_files()


@pytest.fixture
def client():
    """Create test client."""
    return TestClient(app)


def random_string(length: int = 10) -> str:
    return "".join(random.choices(string.ascii_letters, k=length))


def random_email() -> str:
    return f"{random_string(8)}@{random_string(5)}.com"


def random_file_content(size_kb: int = 1) -> bytes:
    """Generate random file content."""
    return os.urandom(size_kb * 1024)


def create_file_tuple(
    filename: str, content: bytes, content_type: str = "application/octet-stream"
):
    """Create a file tuple for multipart upload."""
    return (filename, io.BytesIO(content), content_type)


class TestFormData:
    """Test form data handling."""

    def test_simple_form(self, client):
        """Test simple form submission."""
        response = client.post(
            "/form/simple",
            data={
                "username": random_string(10),
                "email": random_email(),
            },
        )
        assert response.status_code == 200
        data = response.json()
        assert "username" in data
        assert "email" in data

    def test_simple_form_validation(self, client):
        """Test form validation."""
        # Username too short
        response = client.post(
            "/form/simple",
            data={
                "username": "ab",  # min 3
                "email": random_email(),
            },
        )
        assert response.status_code == 422

        # Missing required field
        response = client.post(
            "/form/simple",
            data={"username": random_string(10)},
        )
        assert response.status_code == 422

    def test_complex_form(self, client):
        """Test complex form with optional fields."""
        username = random_string(10)
        email = random_email()
        age = random.randint(18, 80)
        bio = random_string(100)

        response = client.post(
            "/form/complex",
            data={
                "username": username,
                "email": email,
                "age": age,
                "bio": bio,
            },
        )
        assert response.status_code == 200
        data = response.json()
        assert data["username"] == username
        assert data["email"] == email
        assert data["age"] == age
        assert data["bio"] == bio

    def test_complex_form_optional_fields(self, client):
        """Test complex form with only required fields."""
        response = client.post(
            "/form/complex",
            data={
                "username": random_string(10),
                "email": random_email(),
            },
        )
        assert response.status_code == 200
        data = response.json()
        assert data["age"] is None
        assert data["bio"] is None


class TestSingleFileUpload:
    """Test single file upload."""

    def test_upload_single_file(self, client):
        """Test uploading a single file."""
        content = random_file_content(5)
        filename = f"{random_string(8)}.bin"

        response = client.post(
            "/upload/single",
            files={"file": (filename, io.BytesIO(content), "application/octet-stream")},
        )
        assert response.status_code == 200
        data = response.json()
        assert data["filename"] == filename
        assert data["size"] == len(content)
        assert data["md5_hash"] == hashlib.md5(content).hexdigest()
        assert "id" in data

    def test_upload_text_file(self, client):
        """Test uploading a text file."""
        content = random_string(500).encode()
        filename = f"{random_string(8)}.txt"

        response = client.post(
            "/upload/single",
            files={"file": (filename, io.BytesIO(content), "text/plain")},
        )
        assert response.status_code == 200
        data = response.json()
        assert data["content_type"] == "text/plain"

    def test_upload_optional_file_present(self, client):
        """Test optional file when provided."""
        content = random_file_content(1)

        response = client.post(
            "/upload/optional",
            files={
                "file": ("test.bin", io.BytesIO(content), "application/octet-stream")
            },
        )
        assert response.status_code == 200
        data = response.json()
        assert data["has_file"] is True
        assert data["size"] == len(content)

    def test_upload_optional_file_missing(self, client):
        """Test optional file when not provided."""
        response = client.post("/upload/optional")
        assert response.status_code == 200
        data = response.json()
        assert data["has_file"] is False


class TestMultipleFileUpload:
    """Test multiple file upload."""

    def test_upload_multiple_files(self, client):
        """Test uploading multiple files."""
        num_files = random.randint(2, 5)
        files = []
        expected_sizes = []

        for i in range(num_files):
            content = random_file_content(random.randint(1, 10))
            expected_sizes.append(len(content))
            files.append(
                (
                    "files",
                    (f"file_{i}.bin", io.BytesIO(content), "application/octet-stream"),
                )
            )

        response = client.post("/upload/multiple", files=files)
        assert response.status_code == 200
        data = response.json()
        assert data["count"] == num_files
        assert len(data["files"]) == num_files

        # Check sizes match
        actual_sizes = [f["size"] for f in data["files"]]
        assert sorted(actual_sizes) == sorted(expected_sizes)

    def test_upload_many_files(self, client):
        """Test uploading many files."""
        num_files = 10
        files = []

        for i in range(num_files):
            content = random_file_content(1)
            files.append(
                (
                    "files",
                    (f"file_{i}.bin", io.BytesIO(content), "application/octet-stream"),
                )
            )

        response = client.post("/upload/multiple", files=files)
        assert response.status_code == 200
        assert response.json()["count"] == num_files


class TestMixedUpload:
    """Test mixed form data and file uploads."""

    def test_file_with_form(self, client):
        """Test uploading a file with form data."""
        title = random_string(20)
        description = random_string(100)
        content = random_file_content(5)

        response = client.post(
            "/upload/with-form",
            data={
                "title": title,
                "description": description,
            },
            files={
                "file": ("test.bin", io.BytesIO(content), "application/octet-stream")
            },
        )
        assert response.status_code == 200
        data = response.json()
        assert data["title"] == title
        assert data["description"] == description
        assert data["file"]["size"] == len(content)

    def test_multiple_files_with_form(self, client):
        """Test uploading multiple files with form data."""
        title = random_string(20)
        tags = [random_string(5) for _ in range(3)]

        files = [
            (
                "files",
                (
                    f"file_{i}.bin",
                    io.BytesIO(random_file_content(1)),
                    "application/octet-stream",
                ),
            )
            for i in range(3)
        ]

        response = client.post(
            "/upload/multiple-with-form",
            data={
                "title": title,
                "tags": tags,
            },
            files=files,
        )
        assert response.status_code == 200
        data = response.json()
        assert data["title"] == title
        assert len(data["files"]) == 3


class TestFileValidation:
    """Test file validation."""

    def test_upload_valid_image(self, client):
        """Test uploading a valid image type."""
        # Create minimal valid PNG header
        png_header = bytes(
            [
                0x89,
                0x50,
                0x4E,
                0x47,
                0x0D,
                0x0A,
                0x1A,
                0x0A,
                0x00,
                0x00,
                0x00,
                0x0D,
                0x49,
                0x48,
                0x44,
                0x52,
            ]
        )
        content = png_header + random_file_content(1)

        response = client.post(
            "/upload/image",
            files={"file": ("test.png", io.BytesIO(content), "image/png")},
        )
        assert response.status_code == 200

    def test_upload_invalid_image_type(self, client):
        """Test uploading an invalid image type."""
        content = random_file_content(1)

        response = client.post(
            "/upload/image",
            files={"file": ("test.pdf", io.BytesIO(content), "application/pdf")},
        )
        assert response.status_code == 400
        assert "invalid file type" in response.json()["detail"].lower()

    def test_upload_within_size_limit(self, client):
        """Test uploading a file within size limit."""
        content = random_file_content(1)  # 1KB

        response = client.post(
            "/upload/size-limit",
            data={"max_size": 10 * 1024},  # 10KB limit
            files={
                "file": ("test.bin", io.BytesIO(content), "application/octet-stream")
            },
        )
        assert response.status_code == 200

    def test_upload_exceeds_size_limit(self, client):
        """Test uploading a file exceeding size limit."""
        content = random_file_content(100)  # 100KB

        response = client.post(
            "/upload/size-limit",
            data={"max_size": 10 * 1024},  # 10KB limit
            files={
                "file": ("test.bin", io.BytesIO(content), "application/octet-stream")
            },
        )
        assert response.status_code == 400
        assert "too large" in response.json()["detail"].lower()


class TestFileRetrieval:
    """Test file info retrieval."""

    def test_get_file_info(self, client):
        """Test getting uploaded file info."""
        content = random_file_content(5)

        # Upload file
        upload_response = client.post(
            "/upload/single",
            files={
                "file": ("test.bin", io.BytesIO(content), "application/octet-stream")
            },
        )
        file_id = upload_response.json()["id"]

        # Get file info
        response = client.get(f"/files/{file_id}")
        assert response.status_code == 200
        data = response.json()
        assert data["id"] == file_id
        assert data["size"] == len(content)

    def test_get_nonexistent_file(self, client):
        """Test getting non-existent file."""
        response = client.get("/files/nonexistent-id")
        assert response.status_code == 404


class TestUploadFileAttributes:
    """Test UploadFile attributes and methods."""

    def test_upload_file_attributes(self, client):
        """Test UploadFile attributes are accessible."""
        content = random_file_content(1)
        filename = f"{random_string(8)}.bin"
        content_type = "application/octet-stream"

        response = client.post(
            "/upload/attributes",
            files={"file": (filename, io.BytesIO(content), content_type)},
        )
        assert response.status_code == 200
        data = response.json()
        assert data["filename"] == filename
        assert data["content_type"] == content_type
        assert data["size"] == len(content)
        assert data["first_chunk_size"] == min(100, len(content))


class TestComplexScenarios:
    """Test complex upload scenarios."""

    def test_upload_many_files_sequentially(self, client):
        """Test uploading many files sequentially."""
        file_ids = []

        for i in range(10):
            content = random_file_content(random.randint(1, 5))
            response = client.post(
                "/upload/single",
                files={
                    "file": (
                        f"file_{i}.bin",
                        io.BytesIO(content),
                        "application/octet-stream",
                    )
                },
            )
            assert response.status_code == 200
            file_ids.append(response.json()["id"])

        # Verify all files exist
        for file_id in file_ids:
            response = client.get(f"/files/{file_id}")
            assert response.status_code == 200

    def test_large_file_upload(self, client):
        """Test uploading a larger file (1MB)."""
        content = random_file_content(1024)  # 1MB

        response = client.post(
            "/upload/single",
            files={
                "file": ("large.bin", io.BytesIO(content), "application/octet-stream")
            },
        )
        assert response.status_code == 200
        assert response.json()["size"] == len(content)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
