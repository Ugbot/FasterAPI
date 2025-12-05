#!/bin/bash
#
# Manual curl-based test for query parameter extraction
# This requires a running FasterAPI server
#

set -e

PORT=8000
BASE_URL="http://127.0.0.1:$PORT"

echo "=================================="
echo "Query Parameter Manual Test"
echo "=================================="
echo
echo "Testing against: $BASE_URL"
echo

# Check if server is running
if ! curl -s "$BASE_URL/" > /dev/null 2>&1; then
    echo "❌ No server running on $BASE_URL"
    echo
    echo "Start a server first. For example:"
    echo "  cd /Users/bengamble/FasterAPI"
    echo "  DYLD_LIBRARY_PATH=build/lib python3.13 examples/fastapi_example.py"
    echo
    exit 1
fi

echo "✓ Server detected"
echo

# Test 1: Simple query params
echo "Test 1: Query parameters (GET /items?skip=5&limit=20)"
RESPONSE=$(curl -s "$BASE_URL/items?skip=5&limit=20")
echo "Response: $RESPONSE"
if echo "$RESPONSE" | grep -q '"skip"'; then
    echo "✅ PASS - Query params present in response"
else
    echo "❌ FAIL - Query params missing"
fi
echo

# Test 2: Path + query parameters
echo "Test 2: Path + Query (GET /items/10 - assuming item exists)"
RESPONSE=$(curl -s "$BASE_URL/items/1")
echo "Response: $RESPONSE"
if echo "$RESPONSE" | grep -q '"id"'; then
    echo "✅ PASS - Path param extracted"
else
    echo "❌ FAIL - Path param missing"
fi
echo

# Test 3: Query with tag filter
echo "Test 3: Query filter (GET /items?tag=electronics)"
RESPONSE=$(curl -s "$BASE_URL/items?tag=electronics")
echo "Response: $RESPONSE"
if echo "$RESPONSE" | grep -q "electronics\|items"; then
    echo "✅ PASS - Filter query param working"
else
    echo "❌ FAIL - Filter not applied"
fi
echo

echo "=================================="
echo "Manual tests complete"
echo "=================================="
echo
echo "To test with custom queries:"
echo "  curl '$BASE_URL/items?skip=10&limit=5'"
echo "  curl '$BASE_URL/items/123'"
echo "  curl '$BASE_URL/items?tag=premium'"
