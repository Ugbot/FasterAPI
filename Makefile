.PHONY: build build-debug build-pg build-http test bench clean help

# Default target
help:
	@echo "FasterAPI - Makefile Targets"
	@echo ""
	@echo "  build          - Build both C++ libraries (PostgreSQL + HTTP)"
	@echo "  build-pg       - Build PostgreSQL library only"
	@echo "  build-http     - Build HTTP server library only"
	@echo "  build-debug    - Build with debug symbols"
	@echo "  test           - Run integration tests"
	@echo "  bench          - Run performance benchmarks"
	@echo "  lint           - Run Python linting"
	@echo "  example        - Run full integration example"
	@echo "  clean          - Clean build artifacts"
	@echo ""

# Build both libraries (optimized release)
build:
	@echo "Building FasterAPI (PostgreSQL + HTTP)"
	mkdir -p build
	cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DFA_BUILD_PG=ON -DFA_BUILD_HTTP=ON -DFA_ENABLE_HTTP2=ON -DFA_ENABLE_COMPRESSION=ON -Wno-dev
	cd build && cmake --build . --config Release -j
	@echo "✓ Build complete"
	@echo "  PostgreSQL: build/lib/libfasterapi_pg.*"
	@echo "  HTTP:       build/lib/libfasterapi_http.*"
	@echo ""
	@echo "Copying libraries to Python package..."
	@mkdir -p fasterapi/pg/_native fasterapi/http/_native
	@cp build/lib/libfasterapi_pg.* fasterapi/pg/_native/ 2>/dev/null || true
	@cp build/lib/libfasterapi_http.* fasterapi/http/_native/ 2>/dev/null || true
	@echo "✓ Libraries copied"

# Build PostgreSQL library only
build-pg:
	@echo "Building PostgreSQL library only"
	mkdir -p build
	cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DFA_BUILD_PG=ON -DFA_BUILD_HTTP=OFF -Wno-dev
	cd build && cmake --build . --config Release -j
	@mkdir -p fasterapi/pg/_native
	@cp build/lib/libfasterapi_pg.* fasterapi/pg/_native/
	@echo "✓ PostgreSQL library built"

# Build HTTP library only  
build-http:
	@echo "Building HTTP server library only"
	mkdir -p build
	cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DFA_BUILD_PG=OFF -DFA_BUILD_HTTP=ON -DFA_ENABLE_HTTP2=ON -Wno-dev
	cd build && cmake --build . --config Release -j
	@mkdir -p fasterapi/http/_native
	@cp build/lib/libfasterapi_http.* fasterapi/http/_native/
	@echo "✓ HTTP library built"

# Build debug library with symbols
build-debug:
	@echo "Building FasterAPI PostgreSQL (Debug)"
	mkdir -p build-debug
	cd build-debug && cmake .. -DCMAKE_BUILD_TYPE=Debug -Wno-dev
	cd build-debug && cmake --build . --config Debug
	@echo "Build complete: build-debug/lib/libfasterapi_pg.*"

# Run integration tests
test: build
	@echo "Running integration tests"
	pytest tests/integration_test.py -v --tb=short

# Run performance benchmarks
bench: build
	@echo "Running performance benchmarks"
	python benchmarks/runner.py

# Run full integration example
example: build
	@echo "Running full integration example"
	python examples/full_integration.py

# Run Python linting
lint:
	@echo "Running Python linting"
	python -m pylint fasterapi/pg --disable=all --enable=E,F,W || true
	python -m mypy fasterapi/pg --ignore-missing-imports || true

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts"
	rm -rf build build-debug dist build CMakeFiles CMakeCache.txt cmake_install.cmake
	find . -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
	find . -type f -name "*.pyc" -delete
	find . -type f -name "*.so" -delete
	find . -type f -name "*.dylib" -delete
	@echo "Clean complete"

# Development install (editable)
install-dev:
	pip install -e ".[dev]"

# View build output
cat-build:
	@cat build/CMakeFiles/fasterapi_pg.dir/src/cpp/pg/pg_lib.cpp.o.log 2>/dev/null || echo "No build log found"
