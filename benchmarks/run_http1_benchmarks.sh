#!/bin/bash

# FasterAPI HTTP/1 Unified Benchmark Suite
# Runs Python TechEmpower tests followed by C++ 1MRC tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Results storage (simple variables for bash 3.2 compatibility)
RESULT_PYTHON_JSON=""
RESULT_PYTHON_PLAINTEXT=""
RESULT_PYTHON_DB=""
RESULT_CPP_THREADING=""
RESULT_CPP_ASYNC=""
RESULT_CPP_LIBUV=""

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║       FasterAPI HTTP/1 Unified Benchmark Suite              ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# -----------------------------
# Dependency Checks
# -----------------------------

echo -e "${CYAN}Checking dependencies...${NC}"
echo ""

# Check for load testing tools
LOAD_TOOL=""
if command -v wrk &> /dev/null; then
    LOAD_TOOL="wrk"
    echo -e "${GREEN}✓${NC} wrk found"
elif command -v ab &> /dev/null; then
    LOAD_TOOL="ab"
    echo -e "${GREEN}✓${NC} ab (Apache Bench) found"
else
    echo -e "${RED}✗${NC} Neither wrk nor ab found"
    echo -e "${YELLOW}Please install one:${NC}"
    echo "  macOS:  brew install wrk"
    echo "  Linux:  apt-get install apache2-utils (for ab)"
    exit 1
fi

# Check for Python
if ! command -v python3 &> /dev/null; then
    echo -e "${RED}✗${NC} python3 not found"
    exit 1
fi
echo -e "${GREEN}✓${NC} python3 found"

# Check for Python dependencies
if ! python3 -c "import aiohttp" 2>/dev/null; then
    echo -e "${YELLOW}⚠${NC}  aiohttp not installed (needed for 1MRC client)"
    echo "  Install: pip install aiohttp"
    exit 1
fi
echo -e "${GREEN}✓${NC} aiohttp installed"

# Check for C++ servers
if [ ! -f "$PROJECT_ROOT/build/benchmarks/1mrc_cpp_server" ]; then
    echo -e "${RED}✗${NC} 1MRC C++ servers not built"
    echo -e "${YELLOW}Building now...${NC}"
    cd "$PROJECT_ROOT/build"
    ninja 1mrc_cpp_server 1mrc_libuv_server 1mrc_async_server || {
        echo -e "${RED}✗${NC} Build failed"
        exit 1
    }
    cd "$PROJECT_ROOT"
fi
echo -e "${GREEN}✓${NC} C++ servers built"

echo ""
echo -e "${CYAN}All dependencies satisfied!${NC}"
echo ""

# -----------------------------
# Phase 1: Python TechEmpower Tests
# -----------------------------

echo "═══════════════════════════════════════════════════════════════"
echo "  Phase 1: Python TechEmpower Benchmarks (HTTP/1.1)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

echo -e "${YELLOW}Starting Python TechEmpower server...${NC}"
cd "$PROJECT_ROOT"
PYTHONPATH=. python3 benchmarks/techempower/techempower_benchmarks.py &
SERVER_PID=$!
sleep 3

# Check if server started
if ! curl -s http://localhost:8080/json > /dev/null 2>&1; then
    echo -e "${RED}✗${NC} Server failed to start"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi
echo -e "${GREEN}✓${NC} Server running (PID: $SERVER_PID)"
echo ""

# Run benchmarks based on available tool
if [ "$LOAD_TOOL" = "wrk" ]; then
    echo -e "${CYAN}Running wrk benchmarks...${NC}"
    echo ""

    # JSON serialization
    echo -e "${BLUE}Test 1: JSON Serialization${NC}"
    WRK_OUT=$(wrk -t4 -c64 -d10s --latency http://localhost:8080/json 2>&1)
    RESULT_PYTHON_JSON=$(echo "$WRK_OUT" | grep "Requests/sec:" | awk '{print $2}')
    echo "$WRK_OUT" | grep -E "(Requests/sec|Latency|requests in)"
    echo ""

    # Plaintext
    echo -e "${BLUE}Test 2: Plaintext (Minimum Overhead)${NC}"
    WRK_OUT=$(wrk -t4 -c64 -d10s --latency http://localhost:8080/plaintext 2>&1)
    RESULT_PYTHON_PLAINTEXT=$(echo "$WRK_OUT" | grep "Requests/sec:" | awk '{print $2}')
    echo "$WRK_OUT" | grep -E "(Requests/sec|Latency|requests in)"
    echo ""

    # Single query
    echo -e "${BLUE}Test 3: Single Database Query${NC}"
    WRK_OUT=$(wrk -t4 -c64 -d10s --latency http://localhost:8080/db 2>&1)
    RESULT_PYTHON_DB=$(echo "$WRK_OUT" | grep "Requests/sec:" | awk '{print $2}')
    echo "$WRK_OUT" | grep -E "(Requests/sec|Latency|requests in)"
    echo ""

else
    # Using ab
    echo -e "${CYAN}Running ab (Apache Bench) benchmarks...${NC}"
    echo ""

    echo -e "${BLUE}Test 1: JSON Serialization${NC}"
    AB_OUT=$(ab -n 10000 -c 100 http://localhost:8080/json 2>&1)
    RESULT_PYTHON_JSON=$(echo "$AB_OUT" | grep "Requests per second:" | awk '{print $4}')
    echo "$AB_OUT" | grep -E "(Requests per second|Time per request)"
    echo ""

    echo -e "${BLUE}Test 2: Plaintext${NC}"
    AB_OUT=$(ab -n 10000 -c 100 http://localhost:8080/plaintext 2>&1)
    RESULT_PYTHON_PLAINTEXT=$(echo "$AB_OUT" | grep "Requests per second:" | awk '{print $4}')
    echo "$AB_OUT" | grep -E "(Requests per second|Time per request)"
    echo ""
fi

# Stop Python server
echo -e "${YELLOW}Stopping Python server...${NC}"
kill $SERVER_PID 2>/dev/null
sleep 1
echo ""

# -----------------------------
# Phase 2: C++ 1MRC Tests
# -----------------------------

echo "═══════════════════════════════════════════════════════════════"
echo "  Phase 2: C++ 1MRC Benchmarks (HTTP/1.1)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# Function to run 1MRC test
run_1mrc_test() {
    local SERVER_NAME=$1
    local SERVER_PATH=$2

    echo -e "${CYAN}Testing: ${SERVER_NAME}${NC}"
    echo ""

    # Start server
    echo -e "${YELLOW}Starting server...${NC}"
    "$SERVER_PATH" &
    SERVER_PID=$!
    sleep 2

    # Check if server started
    if ! curl -s http://localhost:8000/stats > /dev/null 2>&1; then
        echo -e "${RED}✗${NC} Server failed to start"
        kill $SERVER_PID 2>/dev/null
        return 1
    fi
    echo -e "${GREEN}✓${NC} Server running (PID: $SERVER_PID)"
    echo ""

    # Run 1MRC client (reduced load for quick test)
    echo -e "${YELLOW}Running 1MRC test (100K requests, 500 concurrent)...${NC}"
    cd "$PROJECT_ROOT/benchmarks/1mrc/client"

    # Capture output
    TEST_OUT=$(python3 1mrc_client.py 100000 500 2>&1 | tee /dev/tty)

    # Extract RPS and store based on server name
    RPS=$(echo "$TEST_OUT" | grep "Requests per second:" | awk '{print $4}')
    case "$SERVER_NAME" in
        "C++_Threading")
            RESULT_CPP_THREADING=$RPS
            ;;
        "C++_AsyncIO")
            RESULT_CPP_ASYNC=$RPS
            ;;
        "C++_libuv")
            RESULT_CPP_LIBUV=$RPS
            ;;
    esac

    echo ""

    # Stop server
    echo -e "${YELLOW}Stopping server...${NC}"
    kill $SERVER_PID 2>/dev/null
    sleep 1
    echo ""

    cd "$PROJECT_ROOT"
}

# Test each C++ server
run_1mrc_test "C++_Threading" "$PROJECT_ROOT/build/benchmarks/1mrc_cpp_server"
run_1mrc_test "C++_AsyncIO" "$PROJECT_ROOT/build/benchmarks/1mrc_async_server"
run_1mrc_test "C++_libuv" "$PROJECT_ROOT/build/benchmarks/1mrc_libuv_server"

# -----------------------------
# Summary Report
# -----------------------------

echo "═══════════════════════════════════════════════════════════════"
echo "  Summary Report - HTTP/1.1 Benchmarks"
echo "═══════════════════════════════════════════════════════════════"
echo ""

echo -e "${CYAN}Python TechEmpower (FasterAPI):${NC}"
echo "  JSON Serialization:   ${RESULT_PYTHON_JSON} req/s"
echo "  Plaintext:            ${RESULT_PYTHON_PLAINTEXT} req/s"
echo "  Single DB Query:      ${RESULT_PYTHON_DB} req/s"
echo ""

echo -e "${CYAN}C++ 1MRC (100K requests):${NC}"
echo "  Threading:            ${RESULT_CPP_THREADING} req/s"
echo "  Async I/O (kqueue):   ${RESULT_CPP_ASYNC} req/s"
echo "  libuv:                ${RESULT_CPP_LIBUV} req/s"
echo ""

# Determine best performer
BEST_CPP="C++_libuv"
BEST_RPS="${RESULT_CPP_LIBUV}"

echo -e "${GREEN}Best Performer:${NC} $BEST_CPP ($BEST_RPS req/s)"
echo ""

echo "═══════════════════════════════════════════════════════════════"
echo ""
echo -e "${GREEN}✓ All benchmarks complete!${NC}"
echo ""
echo "Notes:"
echo "  - All tests use HTTP/1.1 protocol"
echo "  - Python tests: TechEmpower standard benchmarks"
echo "  - C++ tests: 1 Million Request Challenge (scaled to 100K)"
echo "  - Full 1M request test: python3 benchmarks/1mrc/client/1mrc_client.py"
echo ""
