"""
FasterAPI PostgreSQL Integration - Setup

Builds the C++ native library and packages it with Python modules.
"""

from setuptools import setup, find_packages
from setuptools.command.build_ext import build_ext
import os
import sys
import subprocess
from pathlib import Path


class CMakeBuildExt(build_ext):
    """Custom build command that uses CMake to build the C++ extension."""
    
    def build_extensions(self):
        """Build C++ library via CMake."""
        build_dir = Path(self.build_temp).absolute()
        source_dir = Path(__file__).parent.absolute()
        install_dir = Path(self.build_lib) / "fasterapi" / "pg" / "_native"
        
        # Create directories
        build_dir.mkdir(parents=True, exist_ok=True)
        install_dir.mkdir(parents=True, exist_ok=True)
        
        # Run CMake configure
        print(f"Configuring CMake in {build_dir}")
        cmake_args = [
            f"-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_INSTALL_PREFIX={install_dir}",
        ]
        
        subprocess.check_call(
            ["cmake", str(source_dir), "-Wno-dev"] + cmake_args,
            cwd=build_dir
        )
        
        # Run CMake build
        print("Building C++ library")
        subprocess.check_call(
            ["cmake", "--build", ".", "--config", "Release", "-j"],
            cwd=build_dir
        )
        
        # Copy library to package
        lib_dir = build_dir / "lib"
        if lib_dir.exists():
            for lib_file in lib_dir.glob("libfasterapi_pg.*"):
                print(f"Copying {lib_file.name} to {install_dir}")
                import shutil
                shutil.copy2(lib_file, install_dir / lib_file.name)


setup(
    name="fasterapi-pg",
    version="0.1.0",
    description="High-performance PostgreSQL driver for FasterAPI with C++ pooling and codecs",
    long_description=open("README.md").read() if Path("README.md").exists() else "",
    author="FasterAPI Contributors",
    license="MIT",
    python_requires=">=3.10",
    packages=find_packages(),
    package_data={
        "fasterapi.pg": ["_native/*"],
    },
    ext_modules=[],  # Custom build_ext handles this
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
            "psycopg[binary]>=3.0",
            "asyncpg>=0.27",
        ],
    },
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Cython :: 0.29",
    ],
)
