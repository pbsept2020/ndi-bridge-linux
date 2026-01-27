#!/bin/bash
#
# build.sh - Build NDI Bridge Linux
#
# Usage:
#   ./scripts/build.sh          # Build in Release mode
#   ./scripts/build.sh debug    # Build in Debug mode
#   ./scripts/build.sh clean    # Clean build directory
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check dependencies
check_dependencies() {
    log_info "Checking dependencies..."

    # CMake
    if ! command -v cmake &> /dev/null; then
        log_error "CMake not found. Install with: sudo apt install cmake"
        exit 1
    fi

    # pkg-config
    if ! command -v pkg-config &> /dev/null; then
        log_error "pkg-config not found. Install with: sudo apt install pkg-config"
        exit 1
    fi

    # FFmpeg libraries
    if ! pkg-config --exists libavcodec libavutil libswscale; then
        log_error "FFmpeg development libraries not found"
        log_error "Install with: sudo apt install libavcodec-dev libavutil-dev libswscale-dev"
        exit 1
    fi

    # NDI SDK
    NDI_FOUND=false
    for path in /usr/include "$HOME/NDI SDK for Linux/include" /opt/ndi-sdk/include; do
        if [ -f "$path/Processing.NDI.Lib.h" ]; then
            NDI_FOUND=true
            log_info "NDI SDK found at: $path"
            break
        fi
    done

    if [ "$NDI_FOUND" = false ]; then
        log_warn "NDI SDK not found in standard locations"
        log_warn "Download from: https://ndi.video/sdk/"
        log_warn "Install to: ~/NDI SDK for Linux/ or /usr/include/"
    fi

    log_info "Dependencies OK"
}

# Clean build
clean_build() {
    log_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    log_info "Clean complete"
}

# Build project
build_project() {
    local build_type="${1:-Release}"

    check_dependencies

    log_info "Building in $build_type mode..."

    # Configure
    cmake -B "$BUILD_DIR" \
          -DCMAKE_BUILD_TYPE="$build_type" \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          "$PROJECT_DIR"

    # Build
    cmake --build "$BUILD_DIR" --parallel "$(nproc)"

    # Link compile_commands.json to project root (for IDE support)
    if [ -f "$BUILD_DIR/compile_commands.json" ]; then
        ln -sf "$BUILD_DIR/compile_commands.json" "$PROJECT_DIR/compile_commands.json"
    fi

    log_info "Build complete!"
    log_info "Binary: $BUILD_DIR/ndi-bridge"
}

# Main
case "${1:-}" in
    clean)
        clean_build
        ;;
    debug)
        build_project Debug
        ;;
    release|"")
        build_project Release
        ;;
    *)
        echo "Usage: $0 [clean|debug|release]"
        exit 1
        ;;
esac
