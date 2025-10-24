#!/bin/bash
# Simple HTTP/2 Async Flow Test using curl

echo "=== Testing HTTP/2 Async Flow with curl ==="
echo

# Test 1: Simple GET request
echo "Test 1: Simple GET /"
curl --http2-prior-knowledge -s http://localhost:8080/ | jq .
echo

# Test 2: Slow endpoint (tests async behavior)
echo "Test 2: Slow GET /slow"
start=$(date +%s.%N)
curl --http2-prior-knowledge -s http://localhost:8080/slow | jq .
end=$(date +%s.%N)
elapsed=$(echo "$end - $start" | bc)
echo "Elapsed: $elapsed seconds"
echo

# Test 3: POST echo
echo "Test 3: POST /echo"
curl --http2-prior-knowledge -s -X POST -d "test message" http://localhost:8080/echo
echo
echo

# Test 4: Concurrent requests (tests wake mechanism)
echo "Test 4: Concurrent requests (10 parallel)"
start=$(date +%s.%N)
for i in {1..10}; do
    curl --http2-prior-knowledge -s http://localhost:8080/slow &
done
wait
end=$(date +%s.%N)
elapsed=$(echo "$end - $start" | bc)
echo
echo "Total time for 10 parallel requests: $elapsed seconds"
echo "Expected: ~0.1-0.3s (parallel execution)"
echo "If ~1.0s: sequential execution (blocking)"
echo

echo "=== Test Complete ==="
