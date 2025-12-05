#!/bin/bash

# FasterAPI Build Script
# Usage: ./build.sh [options]
# Options:
#   --release          Build in Release mode (default: Debug)
#   --asan             Enable AddressSanitizer
#   --tsan             Enable ThreadSanitizer
#   --clean            Clean build directory before building
#   --target <name>    Build specific target (default: all)
#   --help             Show this help message

set -e  # Exit on error

# Default values
BUILD_TYPE="Debug"
ENABLE_ASAN=0
ENABLE_TSAN=0
CLEAN_BUILD=0
TARGET="all"
BUILD_DIR="build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --asan)
            ENABLE_ASAN=1
            shift
            ;;
        --tsan)
            ENABLE_TSAN=1
            shift
            ;;
        --clean)
            CLEAN_BUILD=1
            shift
            ;;
        --target)
            TARGET="$2"
            shift 2
            ;;
        --help)
            echo "FasterAPI Build Script"
            echo ""
            echo "Usage: ./build.sh [options]"
            echo ""
            echo "Options:"
            echo "  --release          Build in Release mode (default: Debug)"
            echo "  --asan             Enable AddressSanitizer"
            echo "  --tsan             Enable ThreadSanitizer"
            echo "  --clean            Clean build directory before building"
            echo "  --target <name>    Build specific target (default: all)"
            echo "  --help             Show this help message"
            echo ""
            echo "Examples:"
            echo "  ./build.sh                              # Debug build"
            echo "  ./build.sh --release                    # Release build"
            echo "  ./build.sh --asan                       # Debug with ASAN"
            echo "  ./build.sh --release --asan             # Release with ASAN"
            echo "  ./build.sh --tsan                       # Debug with TSAN"
            echo "  ./build.sh --target test_app_destructor # Build specific test"
            echo "  ./build.sh --clean --asan               # Clean build with ASAN"
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Validate: can't use ASAN and TSAN together
if [[ $ENABLE_ASAN -eq 1 && $ENABLE_TSAN -eq 1 ]]; then
    echo -e "${RED}Error: Cannot enable both ASAN and TSAN simultaneously${NC}"
    exit 1
fi

# Use different build directory for sanitizer builds
if [[ $ENABLE_ASAN -eq 1 ]]; then
    BUILD_DIR="build-asan"
elif [[ $ENABLE_TSAN -eq 1 ]]; then
    BUILD_DIR="build-tsan"
fi

# Print build configuration
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}FasterAPI Build Configuration${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "Build Directory:   ${GREEN}${BUILD_DIR}${NC}"
echo -e "Build Type:        ${GREEN}${BUILD_TYPE}${NC}"
echo -e "ASAN:              $([ $ENABLE_ASAN -eq 1 ] && echo -e "${GREEN}Enabled${NC}" || echo -e "${YELLOW}Disabled${NC}")"
echo -e "TSAN:              $([ $ENABLE_TSAN -eq 1 ] && echo -e "${GREEN}Enabled${NC}" || echo -e "${YELLOW}Disabled${NC}")"
echo -e "Target:            ${GREEN}${TARGET}${NC}"
echo -e "Clean Build:       $([ $CLEAN_BUILD -eq 1 ] && echo -e "${GREEN}Yes${NC}" || echo -e "${YELLOW}No${NC}")"
echo -e "${BLUE}========================================${NC}"
echo ""

# Clean build directory if requested
if [[ $CLEAN_BUILD -eq 1 ]]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    if [[ -d "$BUILD_DIR" ]]; then
        rm -rf "$BUILD_DIR"/*
        rm -rf "$BUILD_DIR"/.[!.]*  # Remove hidden files except . and ..
    fi
    echo -e "${GREEN}✓ Build directory cleaned${NC}"
    echo ""
fi

# Create build directory if it doesn't exist
if [[ ! -d "$BUILD_DIR" ]]; then
    echo -e "${YELLOW}Creating build directory...${NC}"
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Prepare CMake arguments as array
CMAKE_ARGS=(
    "-G" "Ninja"
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
)

# Add sanitizer flags if requested
if [[ $ENABLE_ASAN -eq 1 ]]; then
    echo -e "${YELLOW}Configuring AddressSanitizer...${NC}"
    CMAKE_ARGS+=(
        "-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g"
        "-DCMAKE_C_FLAGS=-fsanitize=address -fno-omit-frame-pointer -g"
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address"
        "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address"
        "-DFA_ENABLE_MIMALLOC=OFF"
    )
fi

if [[ $ENABLE_TSAN -eq 1 ]]; then
    echo -e "${YELLOW}Configuring ThreadSanitizer...${NC}"
    CMAKE_ARGS+=(
        "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -g"
        "-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -g"
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"
        "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread"
        "-DFA_ENABLE_MIMALLOC=OFF"
    )
fi

# Run CMake configuration if needed
if [[ ! -f "build.ninja" ]] || [[ $CLEAN_BUILD -eq 1 ]]; then
    echo -e "${YELLOW}Running CMake configuration...${NC}"
    cmake "${CMAKE_ARGS[@]}" ..
    if [[ $? -eq 0 ]]; then
        echo -e "${GREEN}✓ CMake configuration successful${NC}"
        echo ""
    else
        echo -e "${RED}✗ CMake configuration failed${NC}"
        exit 1
    fi
fi

# Build the target
echo -e "${YELLOW}Building target: ${TARGET}...${NC}"
ninja -v "$TARGET"

if [[ $? -eq 0 ]]; then
    echo ""
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}✓ Build successful!${NC}"
    echo -e "${GREEN}========================================${NC}"

    # Show binary location if building specific target
    if [[ "$TARGET" != "all" ]]; then
        if [[ -f "tests/$TARGET" ]]; then
            echo -e "Binary location: ${BLUE}build/tests/$TARGET${NC}"
        elif [[ -f "$TARGET" ]]; then
            echo -e "Binary location: ${BLUE}build/$TARGET${NC}"
        fi
    fi
else
    echo ""
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}✗ Build failed${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi
