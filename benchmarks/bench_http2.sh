#!/bin/bash

URL="http://localhost:8080/"
CONCURRENT=100
DURATION=10
REQUESTS_PER_WORKER=1000

echo "Benchmarking HTTP/2 server at $URL"
echo "Concurrency: $CONCURRENT workers"
echo "Duration: ~${DURATION}s"
echo "Starting benchmark..."

START=$(date +%s)
COUNT=0

# Run concurrent curl workers in background
for i in $(seq 1 $CONCURRENT); do
  {
    for j in $(seq 1 $REQUESTS_PER_WORKER); do
      curl -s --http2-prior-knowledge "$URL" > /dev/null 2>&1 && ((COUNT++))
    done
  } &
done

# Wait for all background jobs
wait

END=$(date +%s)
ELAPSED=$((END - START))

echo ""
echo "Results:"
echo "Total requests: $((CONCURRENT * REQUESTS_PER_WORKER))"
echo "Successful: $COUNT"
echo "Time elapsed: ${ELAPSED}s"
echo "Requests/sec: $(echo "scale=2; $((CONCURRENT * REQUESTS_PER_WORKER)) / $ELAPSED" | bc)"
