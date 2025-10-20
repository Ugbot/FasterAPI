#!/bin/bash

# TechEmpower Benchmark Test Runner
# Runs standard benchmarks using wrk or ab

echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║       FasterAPI - TechEmpower Benchmark Tests                 ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

# Check for wrk
if command -v wrk &> /dev/null; then
    TOOL="wrk"
    echo "✅ Using wrk for benchmarking"
elif command -v ab &> /dev/null; then
    TOOL="ab"
    echo "✅ Using ab for benchmarking"
else
    echo "❌ Neither wrk nor ab found. Please install one:"
    echo "   brew install wrk  (macOS)"
    echo "   apt-get install apache2-utils  (Linux, for ab)"
    exit 1
fi

echo ""

# Start server in background
echo "🚀 Starting FasterAPI server..."
cd "$(dirname "$0")/.."
PYTHONPATH=. python3 benchmarks/techempower_benchmarks.py &
SERVER_PID=$!
sleep 2

# Check if server started
if ! curl -s http://localhost:8080/json > /dev/null 2>&1; then
    echo "❌ Server failed to start"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi

echo "✅ Server started (PID: $SERVER_PID)"
echo ""

# Run benchmarks
echo "═══════════════════════════════════════════════════════════════"
echo "Running TechEmpower Benchmarks..."
echo "═══════════════════════════════════════════════════════════════"
echo ""

if [ "$TOOL" = "wrk" ]; then
    # Test 1: JSON Serialization
    echo "1️⃣  JSON Serialization Test"
    wrk -t4 -c64 -d10s --latency http://localhost:8080/json 2>&1 | grep -E "(Requests/sec|Latency|requests in)"
    echo ""
    
    # Test 2: Plaintext
    echo "2️⃣  Plaintext Test (Minimum Overhead)"
    wrk -t4 -c64 -d10s --latency http://localhost:8080/plaintext 2>&1 | grep -E "(Requests/sec|Latency|requests in)"
    echo ""
    
    # Test 3: Single Query
    echo "3️⃣  Single Database Query"
    wrk -t4 -c64 -d10s --latency http://localhost:8080/db 2>&1 | grep -E "(Requests/sec|Latency|requests in)"
    echo ""
    
    # Test 4: Multiple Queries
    echo "4️⃣  Multiple Queries (20 queries)"
    wrk -t4 -c64 -d10s --latency "http://localhost:8080/queries?queries=20" 2>&1 | grep -E "(Requests/sec|Latency|requests in)"
    echo ""
    
    # Test 5: Updates
    echo "5️⃣  Database Updates (20 updates)"
    wrk -t4 -c64 -d10s --latency "http://localhost:8080/updates?queries=20" 2>&1 | grep -E "(Requests/sec|Latency|requests in)"
    echo ""
    
    # Test 6: Fortunes
    echo "6️⃣  Fortunes (Server-side rendering)"
    wrk -t4 -c64 -d10s --latency http://localhost:8080/fortunes 2>&1 | grep -E "(Requests/sec|Latency|requests in)"
    echo ""
    
else
    # Using ab
    echo "1️⃣  JSON Serialization Test"
    ab -n 10000 -c 100 http://localhost:8080/json 2>&1 | grep -E "(Requests per second|Time per request|across all)"
    echo ""
    
    echo "2️⃣  Plaintext Test"
    ab -n 10000 -c 100 http://localhost:8080/plaintext 2>&1 | grep -E "(Requests per second|Time per request|across all)"
    echo ""
    
    echo "3️⃣  Single Database Query"
    ab -n 10000 -c 100 http://localhost:8080/db 2>&1 | grep -E "(Requests per second|Time per request|across all)"
    echo ""
fi

echo "═══════════════════════════════════════════════════════════════"
echo ""

# Stop server
echo "🛑 Stopping server..."
kill $SERVER_PID 2>/dev/null
sleep 1

echo ""
echo "🎉 TechEmpower benchmarks complete!"
echo ""
echo "💡 To run full TechEmpower suite:"
echo "   1. Add FasterAPI to TechEmpower repo"
echo "   2. Run: ./tfb --mode verify --test fasterapi"
echo ""

