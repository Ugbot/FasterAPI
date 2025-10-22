"""
FasterAPI - Setup

Builds the C++ native library and Cython extensions, packages with Python modules.
"""

from setuptools import setup, find_packages, Extension
from setuptools.command.build_ext import build_ext
import os
import sys
import subprocess
from pathlib import Path

# Check if Cython is available
try:
    from Cython.Build import cythonize
    HAS_CYTHON = True
except ImportError:
    HAS_CYTHON = False
    print("Warning: Cython not available. MCP proxy bindings will not be built.")


class CMakeBuildExt(build_ext):
    """Custom build command that uses CMake to build the C++ extension."""

    def run(self):
        """Build C++ library via CMake, then build Cython extensions."""
        # Build C++ library first
        self.build_cpp_library()

        # Then build Cython extensions if available
        if HAS_CYTHON:
            super().run()

    def build_cpp_library(self):
        """Build C++ library via CMake."""
        build_dir = Path(self.build_temp).absolute()
        source_dir = Path(__file__).parent.absolute()

        # Target directory for native libraries
        native_dir = source_dir / "fasterapi" / "_native"
        native_dir.mkdir(parents=True, exist_ok=True)

        # Create build directory
        build_dir.mkdir(parents=True, exist_ok=True)

        # Run CMake configure
        print(f"Configuring CMake in {build_dir}")
        cmake_args = [
            f"-DCMAKE_BUILD_TYPE=Release",
            f"-DFA_BUILD_MCP=OFF",  # Disable MCP (has exception issues)
            f"-DFA_BUILD_PG=ON",   # Enable PostgreSQL
            f"-DFA_BUILD_HTTP=ON", # Enable HTTP
            f"-DFA_BUILD_BENCHMARKS=OFF",  # Disable benchmarks for faster build
        ]

        subprocess.check_call(
            ["cmake", "-G", "Ninja", str(source_dir), "-Wno-dev"] + cmake_args,
            cwd=build_dir
        )

        # Run Ninja build
        print("Building C++ library with Ninja")
        subprocess.check_call(
            ["ninja", "fasterapi_http", "fasterapi_pg"],
            cwd=build_dir
        )

        # Copy libraries to fasterapi/_native/
        import shutil

        # Copy MCP library
        for pattern in ["libfasterapi_mcp.*", "fasterapi_mcp.*"]:
            for lib_file in build_dir.rglob(pattern):
                if lib_file.is_file() and not lib_file.suffix in ['.a', '.lib']:
                    print(f"Copying {lib_file.name} to {native_dir}")
                    shutil.copy2(lib_file, native_dir / lib_file.name)

        # Copy PG library
        for pattern in ["libfasterapi_pg.*", "fasterapi_pg.*"]:
            for lib_file in build_dir.rglob(pattern):
                if lib_file.is_file() and not lib_file.suffix in ['.a', '.lib']:
                    print(f"Copying {lib_file.name} to {native_dir}")
                    shutil.copy2(lib_file, native_dir / lib_file.name)

        # Copy HTTP library
        for pattern in ["libfasterapi_http.*", "fasterapi_http.*"]:
            for lib_file in build_dir.rglob(pattern):
                if lib_file.is_file() and not lib_file.suffix in ['.a', '.lib']:
                    print(f"Copying {lib_file.name} to {native_dir}")
                    shutil.copy2(lib_file, native_dir / lib_file.name)

        # Copy CoroIO library
        for pattern in ["libcoroio.*", "coroio.*"]:
            for lib_file in build_dir.rglob(pattern):
                if lib_file.is_file() and not lib_file.suffix in ['.a', '.lib']:
                    print(f"Copying {lib_file.name} to {native_dir}")
                    shutil.copy2(lib_file, native_dir / lib_file.name)


# Cython extensions
extensions = []

if HAS_CYTHON:
    # HTTP Server bindings (Cython - high performance)
    extensions.append(
        Extension(
            "fasterapi.http.server_cy",
            sources=["fasterapi/http/server_cy.pyx"],
            include_dirs=[".", "src/cpp", "external/coroio"],
            library_dirs=["fasterapi/_native", "build/lib", "build/external/coroio/coroio"],
            libraries=["fasterapi_http", "coroio"],
            language="c++",
            extra_compile_args=["-std=c++20", "-fexceptions"],  # CoroIO needs exceptions
        )
    )

    # MCP Proxy bindings - DISABLED temporarily due to NULL pointer issues
    # extensions.append(
    #     Extension(
    #         "fasterapi.mcp.proxy_bindings",
    #         sources=["fasterapi/mcp/proxy_bindings.pyx"],
    #         include_dirs=["src/cpp"],
    #         library_dirs=["fasterapi/_native"],
    #         libraries=["fasterapi_mcp"],
    #         language="c++",
    #         extra_compile_args=["-std=c++20"],
    #     )
    # )

    # Cythonize extensions
    extensions = cythonize(
        extensions,
        compiler_directives={
            'language_level': 3,
            'embedsignature': True
        }
    )


setup(
    name="fasterapi",
    version="0.2.0",
    description="High-performance web framework with PostgreSQL and MCP support",
    long_description=open("README.md").read() if Path("README.md").exists() else "",
    long_description_content_type="text/markdown",
    author="FasterAPI Contributors",
    url="https://github.com/bengamble/FasterAPI",
    license="MIT",
    python_requires=">=3.8",
    packages=find_packages(exclude=["tests", "tests.*", "benchmarks", "benchmarks.*"]),
    package_data={
        "fasterapi": ["_native/*"],
        "fasterapi.mcp": ["*.pyx", "*.pxd"],
    },
    include_package_data=True,
    ext_modules=extensions,
    cmdclass={
        "build_ext": CMakeBuildExt,
    },
    install_requires=[
        "pydantic>=2.0",
    ],
    extras_require={
        "dev": [
            "pytest>=7.0",
            "pytest-cov>=4.0",
            "pytest-asyncio>=0.21",
            "cython>=3.0",
        ],
        "pg": [
            "psycopg[binary]>=3.0",
            "asyncpg>=0.27",
        ],
        "mcp": [
            "cython>=3.0",
        ],
        "all": [
            "cython>=3.0",
            "psycopg[binary]>=3.0",
            "asyncpg>=0.27",
            "pytest>=7.0",
            "pytest-cov>=4.0",
            "pytest-asyncio>=0.21",
        ],
    },
    entry_points={
        "console_scripts": [
            "fasterapi-mcp-proxy=fasterapi.mcp.cli:main",
        ],
    },
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: C++",
        "Programming Language :: Cython",
        "Topic :: Software Development :: Libraries :: Application Frameworks",
        "Topic :: Internet :: WWW/HTTP",
        "Topic :: Database",
    ],
)
