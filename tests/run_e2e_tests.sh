#!/bin/bash
#
# End-to-End Test Runner
# Runs all E2E tests for FasterAPI (C++ and Python layers)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "================================================================================"
echo "FasterAPI End-to-End Test Suite"
echo "================================================================================"
echo ""

# Check if library is built
if [ ! -f "$PROJECT_ROOT/build/lib/libfasterapi_http.dylib" ]; then
    echo -e "${RED}❌ Error: libfasterapi_http.dylib not found${NC}"
    echo "Please build the project first:"
    echo "  cd $PROJECT_ROOT/build && ninja fasterapi_http"
    exit 1
fi

# Set library path
export DYLD_LIBRARY_PATH="$PROJECT_ROOT/build/lib:$DYLD_LIBRARY_PATH"

# Track results
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

run_test() {
    local test_name="$1"
    local test_script="$2"

    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    echo ""
    echo "--------------------------------------------------------------------------------"
    echo -e "${BLUE}Running: $test_name${NC}"
    echo "--------------------------------------------------------------------------------"

    if python3.13 "$test_script" 2>&1; then
        echo -e "${GREEN}✅ $test_name PASSED${NC}"
        PASSED_TESTS=$((PASSED_TESTS + 1))
        return 0
    else
        echo -e "${RED}❌ $test_name FAILED${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        return 1
    fi
}

# Run C++ unit tests
echo ""
echo "================================================================================"
echo "Phase 1: C++ Unit Tests"
echo "================================================================================"

if [ -f "$PROJECT_ROOT/build/tests/test_parameter_extractor" ]; then
    echo ""
    echo "Running C++ parameter extraction tests..."
    if "$PROJECT_ROOT/build/tests/test_parameter_extractor"; then
        echo -e "${GREEN}✅ C++ unit tests PASSED${NC}"
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        echo -e "${RED}❌ C++ unit tests FAILED${NC}"
        TOTAL_TESTS=$((TOTAL_TESTS + 1))
        FAILED_TESTS=$((FAILED_TESTS + 1))
    fi
else
    echo -e "${YELLOW}⚠️  Warning: C++ test executable not found${NC}"
fi

# Run E2E tests
echo ""
echo "================================================================================"
echo "Phase 2: End-to-End Tests"
echo "================================================================================"

# C++ API E2E tests
run_test "C++ API E2E Tests" "$SCRIPT_DIR/e2e_cpp_api_test.py"

# Python API E2E tests
run_test "Python API E2E Tests" "$SCRIPT_DIR/e2e_python_api_test.py"

# Parameter fix verification
run_test "Parameter Extraction Fix Verification" "$SCRIPT_DIR/verify_param_fix.py"

# Final summary
echo ""
echo "================================================================================"
echo "Test Suite Summary"
echo "================================================================================"
echo ""
echo -e "Total Tests:  $TOTAL_TESTS"
echo -e "${GREEN}Passed:       $PASSED_TESTS${NC}"
echo -e "${RED}Failed:       $FAILED_TESTS${NC}"
echo ""

if [ $FAILED_TESTS -eq 0 ]; then
    SUCCESS_RATE="100.0"
else
    SUCCESS_RATE=$(echo "scale=1; 100 * $PASSED_TESTS / $TOTAL_TESTS" | bc)
fi

echo -e "Success Rate: ${SUCCESS_RATE}%"
echo ""
echo "================================================================================"

if [ $FAILED_TESTS -eq 0 ]; then
    echo -e "${GREEN}✅ All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}❌ Some tests failed${NC}"
    exit 1
fi
