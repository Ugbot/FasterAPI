#!/usr/bin/env bash
#
# TechEmpower Framework Benchmarks - Local Runner
#
# Replicates the exact methodology from TFBVerifier:
# https://github.com/TechEmpower/TFBVerifier
#
# Parameters match official TFB:
# - JSON: concurrency 16,32,64,128,256,512 | duration 15s
# - Plaintext: pipeline 256,1024,4096,16384 | depth 16 | duration 15s
#
# Usage: ./tfb_benchmark.sh <server_url> [test_type]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIPELINE_LUA="$SCRIPT_DIR/pipeline.lua"

# TFB Default Parameters
DURATION=15
WARMUP_DURATION=15
PRIMER_DURATION=5
PRIMER_CONCURRENCY=8
TIMEOUT=8

# Get number of CPUs
CPUS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)

print_header() {
    echo ""
    echo "╔══════════════════════════════════════════════════════════════════╗"
    echo "║     TechEmpower Framework Benchmarks - Local Runner              ║"
    echo "╚══════════════════════════════════════════════════════════════════╝"
    echo ""
    echo "Methodology: https://github.com/TechEmpower/TFBVerifier"
    echo "CPUs: $CPUS"
    echo ""
}

check_wrk() {
    if ! command -v wrk &> /dev/null; then
        echo "Error: wrk not found"
        echo "Install with: brew install wrk (macOS) or build from source"
        exit 1
    fi
}

check_server() {
    local url=$1
    echo "Checking server at $url..."
    if ! curl -s --connect-timeout 5 "$url" > /dev/null 2>&1; then
        echo "Error: Server not responding at $url"
        exit 1
    fi
    echo "Server is responding"
}

get_threads() {
    local concurrency=$1
    if [ $concurrency -lt $CPUS ]; then
        echo $concurrency
    else
        echo $CPUS
    fi
}

run_wrk() {
    local url=$1
    local duration=$2
    local concurrency=$3
    local threads=$(get_threads $concurrency)
    local accept_header=$4

    wrk -t$threads -c$concurrency -d${duration}s --timeout $TIMEOUT \
        -H "Host: tfb-server" \
        -H "Accept: $accept_header" \
        -H "Connection: keep-alive" \
        --latency \
        "$url" 2>&1
}

run_wrk_pipeline() {
    local url=$1
    local duration=$2
    local concurrency=$3
    local depth=$4
    local threads=$(get_threads $concurrency)

    wrk -t$threads -c$concurrency -d${duration}s --timeout $TIMEOUT \
        -H "Host: tfb-server" \
        -H "Accept: text/plain,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7" \
        -H "Connection: keep-alive" \
        --latency \
        -s "$PIPELINE_LUA" \
        "$url" -- $depth 2>&1
}

extract_rps() {
    echo "$1" | grep "Requests/sec:" | awk '{print $2}'
}

extract_latency_avg() {
    echo "$1" | grep "Latency" | head -1 | awk '{print $2}'
}

extract_latency_p99() {
    echo "$1" | grep "99%" | awk '{print $2}'
}

benchmark_json() {
    local base_url=$1
    local url="$base_url/json"
    local accept="application/json,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7"

    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo " JSON Serialization Test"
    echo "═══════════════════════════════════════════════════════════════"
    echo "URL: $url"
    echo "Concurrency levels: 16 32 64 128 256 512"
    echo ""

    # Primer
    echo "Running primer (${PRIMER_DURATION}s, c=${PRIMER_CONCURRENCY})..."
    run_wrk "$url" $PRIMER_DURATION $PRIMER_CONCURRENCY "$accept" > /dev/null 2>&1

    # Warmup
    echo "Running warmup (${WARMUP_DURATION}s, c=512)..."
    run_wrk "$url" $WARMUP_DURATION 512 "$accept" > /dev/null 2>&1

    # Benchmark runs
    echo ""
    echo "Concurrency | Requests/sec | Avg Latency | p99 Latency"
    echo "------------|--------------|-------------|------------"

    for concurrency in 16 32 64 128 256 512; do
        output=$(run_wrk "$url" $DURATION $concurrency "$accept")

        rps=$(extract_rps "$output")
        lat_avg=$(extract_latency_avg "$output")
        lat_p99=$(extract_latency_p99 "$output")

        printf "%11d | %12s | %11s | %s\n" $concurrency "$rps" "$lat_avg" "$lat_p99"
    done

    echo ""
}

benchmark_plaintext() {
    local base_url=$1
    local url="$base_url/plaintext"
    local depth=16

    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo " Plaintext Test (Pipelined, depth=$depth)"
    echo "═══════════════════════════════════════════════════════════════"
    echo "URL: $url"
    echo "Pipeline concurrency levels: 256 1024 4096 16384"
    echo ""

    # Primer
    echo "Running primer (${PRIMER_DURATION}s, c=${PRIMER_CONCURRENCY})..."
    run_wrk_pipeline "$url" $PRIMER_DURATION $PRIMER_CONCURRENCY $depth > /dev/null 2>&1

    # Warmup
    echo "Running warmup (${WARMUP_DURATION}s, c=16384)..."
    run_wrk_pipeline "$url" $WARMUP_DURATION 16384 $depth > /dev/null 2>&1

    # Benchmark runs
    echo ""
    echo "Concurrency | Requests/sec | Avg Latency | p99 Latency"
    echo "------------|--------------|-------------|------------"

    for concurrency in 256 1024 4096 16384; do
        output=$(run_wrk_pipeline "$url" $DURATION $concurrency $depth)

        rps=$(extract_rps "$output")
        lat_avg=$(extract_latency_avg "$output")
        lat_p99=$(extract_latency_p99 "$output")

        printf "%11d | %12s | %11s | %s\n" $concurrency "$rps" "$lat_avg" "$lat_p99"
    done

    echo ""
}

usage() {
    echo "Usage: $0 <server_url> [test_type]"
    echo ""
    echo "Test types:"
    echo "  json      - JSON serialization"
    echo "  plaintext - Plaintext with pipelining"
    echo "  all       - Run all tests (default)"
    echo ""
    echo "Examples:"
    echo "  $0 http://localhost:8080"
    echo "  $0 http://localhost:8080 json"
    exit 1
}

main() {
    if [ $# -lt 1 ]; then
        usage
    fi

    local base_url=$1
    local test_type=${2:-all}

    print_header
    check_wrk
    check_server "$base_url"

    case $test_type in
        json)
            benchmark_json "$base_url"
            ;;
        plaintext)
            benchmark_plaintext "$base_url"
            ;;
        all)
            benchmark_json "$base_url"
            benchmark_plaintext "$base_url"
            ;;
        *)
            echo "Unknown test type: $test_type"
            usage
            ;;
    esac

    echo "═══════════════════════════════════════════════════════════════"
    echo " Benchmark Complete"
    echo "═══════════════════════════════════════════════════════════════"
}

main "$@"
