#!/bin/bash
#
# FasterAPI vs FastAPI Performance Benchmark
#
# This script benchmarks FasterAPI against regular FastAPI to measure
# the performance improvement from C++ optimization.
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
REQUESTS=10000
CONCURRENCY=100
WARMUP_REQUESTS=1000
TEST_URL_ROOT="/"
TEST_URL_HEALTH="/health"
TEST_URL_ITEMS="/items"

# Results directory
RESULTS_DIR="benchmark_results"
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

echo "================================================================================"
echo "FasterAPI vs FastAPI Performance Benchmark"
echo "================================================================================"
echo "Requests per test: $REQUESTS"
echo "Concurrency level: $CONCURRENCY"
echo "Warmup requests:   $WARMUP_REQUESTS"
echo "================================================================================"
echo ""

# Function to run benchmark
run_benchmark() {
    local name=$1
    local port=$2
    local url=$3
    local output_file=$4

    echo -e "${BLUE}Testing: $name - $url${NC}"

    # Warmup
    echo -n "  Warming up... "
    ab -n $WARMUP_REQUESTS -c 10 "http://localhost:$port$url" > /dev/null 2>&1 || true
    echo "done"

    # Wait a bit
    sleep 1

    # Actual benchmark
    echo "  Running benchmark..."
    ab -n $REQUESTS -c $CONCURRENCY "http://localhost:$port$url" > "$output_file" 2>&1

    # Extract key metrics
    local rps=$(grep "Requests per second" "$output_file" | awk '{print $4}')
    local mean_time=$(grep "Time per request.*mean\)" "$output_file" | awk '{print $4}')
    local p50=$(grep "50%" "$output_file" | awk '{print $2}')
    local p95=$(grep "95%" "$output_file" | awk '{print $2}')
    local p99=$(grep "99%" "$output_file" | awk '{print $2}')

    echo -e "  ${GREEN}✓ Requests/sec: $rps${NC}"
    echo -e "  ${GREEN}✓ Mean latency: ${mean_time}ms${NC}"
    echo -e "  ${GREEN}✓ P50: ${p50}ms, P95: ${p95}ms, P99: ${p99}ms${NC}"
    echo ""

    # Return metrics for comparison
    echo "$rps|$mean_time|$p50|$p95|$p99"
}

# Function to start server and wait for it
start_server() {
    local name=$1
    local command=$2
    local port=$3
    local pid_file=$4

    echo -e "${YELLOW}Starting $name...${NC}"
    eval "$command" > /dev/null 2>&1 &
    echo $! > "$pid_file"

    # Wait for server to be ready
    echo -n "  Waiting for server on port $port... "
    for i in {1..30}; do
        if curl -s "http://localhost:$port/health" > /dev/null 2>&1; then
            echo "ready"
            return 0
        fi
        sleep 0.5
    done

    echo -e "${RED}failed${NC}"
    return 1
}

# Function to stop server
stop_server() {
    local name=$1
    local pid_file=$2

    if [ -f "$pid_file" ]; then
        local pid=$(cat "$pid_file")
        echo -e "${YELLOW}Stopping $name (PID: $pid)...${NC}"
        kill $pid 2>/dev/null || true
        sleep 2
        kill -9 $pid 2>/dev/null || true
        rm -f "$pid_file"
    fi
}

# Cleanup function
cleanup() {
    echo ""
    echo "Cleaning up..."
    stop_server "FasterAPI" "/tmp/fasterapi.pid"
    stop_server "FastAPI" "/tmp/fastapi.pid"
    # Kill any remaining processes on our ports
    lsof -ti:8000 | xargs kill -9 2>/dev/null || true
    lsof -ti:8001 | xargs kill -9 2>/dev/null || true
}

# Set trap to cleanup on exit
trap cleanup EXIT

# Start FastAPI reference server
echo ""
echo "================================================================================"
echo "1. FastAPI (Reference Implementation)"
echo "================================================================================"
echo ""

start_server "FastAPI" \
    "python3.13 benchmarks/fastapi_reference.py" \
    "8001" \
    "/tmp/fastapi.pid"

sleep 2

# Run FastAPI benchmarks
FASTAPI_ROOT=$(run_benchmark "FastAPI" "8001" "$TEST_URL_ROOT" "$RESULTS_DIR/fastapi_root_${TIMESTAMP}.txt")
FASTAPI_HEALTH=$(run_benchmark "FastAPI" "8001" "$TEST_URL_HEALTH" "$RESULTS_DIR/fastapi_health_${TIMESTAMP}.txt")
FASTAPI_ITEMS=$(run_benchmark "FastAPI" "8001" "$TEST_URL_ITEMS" "$RESULTS_DIR/fastapi_items_${TIMESTAMP}.txt")

stop_server "FastAPI" "/tmp/fastapi.pid"

# Wait a bit
sleep 3

# Start FasterAPI server
echo ""
echo "================================================================================"
echo "2. FasterAPI (C++ Optimized)"
echo "================================================================================"
echo ""

export DYLD_LIBRARY_PATH="$(pwd)/build/lib:$DYLD_LIBRARY_PATH"

start_server "FasterAPI" \
    "DYLD_LIBRARY_PATH=$(pwd)/build/lib:\$DYLD_LIBRARY_PATH python3.13 examples/run_fastapi_server.py" \
    "8000" \
    "/tmp/fasterapi.pid"

sleep 2

# Run FasterAPI benchmarks
FASTERAPI_ROOT=$(run_benchmark "FasterAPI" "8000" "$TEST_URL_ROOT" "$RESULTS_DIR/fasterapi_root_${TIMESTAMP}.txt")
FASTERAPI_HEALTH=$(run_benchmark "FasterAPI" "8000" "$TEST_URL_HEALTH" "$RESULTS_DIR/fasterapi_health_${TIMESTAMP}.txt")
FASTERAPI_ITEMS=$(run_benchmark "FasterAPI" "8000" "$TEST_URL_ITEMS" "$RESULTS_DIR/fasterapi_items_${TIMESTAMP}.txt")

stop_server "FasterAPI" "/tmp/fasterapi.pid"

# Generate comparison report
echo ""
echo "================================================================================"
echo "BENCHMARK RESULTS SUMMARY"
echo "================================================================================"
echo ""

# Function to calculate speedup
calculate_speedup() {
    local fastapi_val=$1
    local fasterapi_val=$2
    echo "scale=2; $fasterapi_val / $fastapi_val" | bc
}

# Parse results
IFS='|' read -r FASTAPI_ROOT_RPS FASTAPI_ROOT_MEAN FASTAPI_ROOT_P50 FASTAPI_ROOT_P95 FASTAPI_ROOT_P99 <<< "$FASTAPI_ROOT"
IFS='|' read -r FASTERAPI_ROOT_RPS FASTERAPI_ROOT_MEAN FASTERAPI_ROOT_P50 FASTERAPI_ROOT_P95 FASTERAPI_ROOT_P99 <<< "$FASTERAPI_ROOT"

IFS='|' read -r FASTAPI_HEALTH_RPS FASTAPI_HEALTH_MEAN FASTAPI_HEALTH_P50 FASTAPI_HEALTH_P95 FASTAPI_HEALTH_P99 <<< "$FASTAPI_HEALTH"
IFS='|' read -r FASTERAPI_HEALTH_RPS FASTERAPI_HEALTH_MEAN FASTERAPI_HEALTH_P50 FASTERAPI_HEALTH_P95 FASTERAPI_HEALTH_P99 <<< "$FASTERAPI_HEALTH"

IFS='|' read -r FASTAPI_ITEMS_RPS FASTAPI_ITEMS_MEAN FASTAPI_ITEMS_P50 FASTAPI_ITEMS_P95 FASTAPI_ITEMS_P99 <<< "$FASTAPI_ITEMS"
IFS='|' read -r FASTERAPI_ITEMS_RPS FASTERAPI_ITEMS_MEAN FASTERAPI_ITEMS_P50 FASTERAPI_ITEMS_P95 FASTERAPI_ITEMS_P99 <<< "$FASTERAPI_ITEMS"

# Calculate speedups
ROOT_SPEEDUP=$(calculate_speedup "$FASTAPI_ROOT_RPS" "$FASTERAPI_ROOT_RPS")
HEALTH_SPEEDUP=$(calculate_speedup "$FASTAPI_HEALTH_RPS" "$FASTERAPI_HEALTH_RPS")
ITEMS_SPEEDUP=$(calculate_speedup "$FASTAPI_ITEMS_RPS" "$FASTERAPI_ITEMS_RPS")

# Print comparison table
printf "%-20s %15s %15s %12s\n" "Endpoint" "FastAPI (req/s)" "FasterAPI (req/s)" "Speedup"
printf "%-20s %15s %15s %12s\n" "--------" "---------------" "----------------" "-------"
printf "%-20s %15s %15s %12s\n" "GET /" "$FASTAPI_ROOT_RPS" "$FASTERAPI_ROOT_RPS" "${ROOT_SPEEDUP}x"
printf "%-20s %15s %15s %12s\n" "GET /health" "$FASTAPI_HEALTH_RPS" "$FASTERAPI_HEALTH_RPS" "${HEALTH_SPEEDUP}x"
printf "%-20s %15s %15s %12s\n" "GET /items" "$FASTAPI_ITEMS_RPS" "$FASTERAPI_ITEMS_RPS" "${ITEMS_SPEEDUP}x"

echo ""
echo "Latency Comparison (ms):"
echo ""
printf "%-20s %10s %10s %10s\n" "GET / (FastAPI)" "$FASTAPI_ROOT_P50" "$FASTAPI_ROOT_P95" "$FASTAPI_ROOT_P99"
printf "%-20s %10s %10s %10s\n" "GET / (FasterAPI)" "$FASTERAPI_ROOT_P50" "$FASTERAPI_ROOT_P95" "$FASTERAPI_ROOT_P99"

echo ""
echo "================================================================================"
echo "Detailed results saved to: $RESULTS_DIR/"
echo "================================================================================"
echo ""

# Save summary to file
SUMMARY_FILE="$RESULTS_DIR/summary_${TIMESTAMP}.txt"
cat > "$SUMMARY_FILE" <<EOF
FasterAPI vs FastAPI Benchmark Results
Generated: $(date)
===========================================

Test Configuration:
- Requests: $REQUESTS
- Concurrency: $CONCURRENCY
- Warmup: $WARMUP_REQUESTS

Results:
--------
GET /:
  FastAPI:   $FASTAPI_ROOT_RPS req/s
  FasterAPI: $FASTERAPI_ROOT_RPS req/s
  Speedup:   ${ROOT_SPEEDUP}x

GET /health:
  FastAPI:   $FASTAPI_HEALTH_RPS req/s
  FasterAPI: $FASTERAPI_HEALTH_RPS req/s
  Speedup:   ${HEALTH_SPEEDUP}x

GET /items:
  FastAPI:   $FASTAPI_ITEMS_RPS req/s
  FasterAPI: $FASTERAPI_ITEMS_RPS req/s
  Speedup:   ${ITEMS_SPEEDUP}x

Latency (P50/P95/P99 in ms):
GET / FastAPI:   $FASTAPI_ROOT_P50 / $FASTAPI_ROOT_P95 / $FASTAPI_ROOT_P99
GET / FasterAPI: $FASTERAPI_ROOT_P50 / $FASTERAPI_ROOT_P95 / $FASTERAPI_ROOT_P99
EOF

echo "Summary also saved to: $SUMMARY_FILE"
echo ""
