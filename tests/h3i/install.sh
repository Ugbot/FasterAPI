#!/bin/bash
#
# h3i Installation Script
#
# Installs Cloudflare's h3i HTTP/3 testing tool from the quiche project.
# Requires Rust/Cargo to be installed.
#
# Usage:
#     ./install.sh [--check-only]
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_status() {
    echo -e "${BLUE}[*]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[+]${NC} $1"
}

print_error() {
    echo -e "${RED}[-]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[!]${NC} $1"
}

# Check for Rust/Cargo
check_rust() {
    if ! command -v cargo &> /dev/null; then
        print_error "Cargo not found!"
        echo ""
        echo "Install Rust via rustup:"
        echo "    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
        echo ""
        return 1
    fi

    RUST_VERSION=$(rustc --version 2>/dev/null | cut -d' ' -f2)
    print_success "Rust found: $RUST_VERSION"
    return 0
}

# Check if h3i is already installed
check_h3i() {
    if command -v h3i &> /dev/null; then
        H3I_VERSION=$(h3i --version 2>/dev/null || echo "unknown")
        print_success "h3i already installed: $H3I_VERSION"
        return 0
    fi
    return 1
}

# Install h3i from crates.io
install_h3i() {
    print_status "Installing h3i from crates.io..."

    # h3i requires quiche which needs cmake and other build tools
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS - check for Xcode command line tools
        if ! xcode-select -p &> /dev/null; then
            print_warning "Xcode command line tools not found. Installing..."
            xcode-select --install 2>/dev/null || true
        fi
    fi

    # Install h3i
    cargo install h3i 2>&1 | while read -r line; do
        echo "    $line"
    done

    if [ ${PIPESTATUS[0]} -ne 0 ]; then
        print_error "h3i installation failed"
        return 1
    fi

    print_success "h3i installed successfully"
    return 0
}

# Verify installation
verify_installation() {
    print_status "Verifying h3i installation..."

    if ! command -v h3i &> /dev/null; then
        # Check if it's in cargo bin directory
        CARGO_BIN="${CARGO_HOME:-$HOME/.cargo}/bin"
        if [ -x "$CARGO_BIN/h3i" ]; then
            print_warning "h3i installed to $CARGO_BIN/h3i"
            print_warning "Add $CARGO_BIN to your PATH"
            export PATH="$CARGO_BIN:$PATH"
        else
            print_error "h3i binary not found after installation"
            return 1
        fi
    fi

    H3I_VERSION=$(h3i --version 2>/dev/null || echo "unknown")
    print_success "h3i verified: $H3I_VERSION"
    return 0
}

# Main
main() {
    echo ""
    echo "================================"
    echo "  h3i Installation Script"
    echo "================================"
    echo ""

    CHECK_ONLY=false
    if [ "$1" == "--check-only" ]; then
        CHECK_ONLY=true
    fi

    # Check Rust
    if ! check_rust; then
        exit 1
    fi

    # Check if already installed
    if check_h3i; then
        if [ "$CHECK_ONLY" = true ]; then
            exit 0
        fi
        print_status "h3i is already installed. Use 'cargo install h3i --force' to reinstall."
        exit 0
    fi

    if [ "$CHECK_ONLY" = true ]; then
        print_warning "h3i not installed"
        exit 1
    fi

    # Install
    if ! install_h3i; then
        exit 1
    fi

    # Verify
    if ! verify_installation; then
        exit 1
    fi

    echo ""
    print_success "Installation complete!"
    echo ""
    echo "Usage:"
    echo "    h3i localhost:4433           # Interactive mode"
    echo "    h3i --help                   # Show all options"
    echo ""
}

main "$@"
