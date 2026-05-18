#!/bin/bash
# Build script for Pinscape Pico Config Tool on Linux
# This script builds the command-line configuration tool for Linux

set -e

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="${SCRIPT_DIR}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}Building Pinscape Pico Config Tool for Linux${NC}"
echo "================================================"
echo ""

# Check for required tools
echo "Checking for required tools..."
for tool in cmake pkg-config; do
    if ! command -v $tool &> /dev/null; then
        echo -e "${RED}Error: $tool is not installed${NC}"
        exit 1
    fi
done

# Check for libusb
echo "Checking for libusb..."
if ! pkg-config --exists libusb-1.0; then
    echo -e "${RED}Error: libusb-1.0 is not installed${NC}"
    echo "On Ubuntu/Debian, install with: sudo apt-get install libusb-1.0-0-dev"
    echo "On Arch Linux, install with: sudo pacman -S libusb"
    echo "On Fedora/RHEL, install with: sudo dnf install libusbx-devel"
    echo "On macOS, install with: brew install libusb"
    exit 1
fi

libusb_version=$(pkg-config --modversion libusb-1.0)
echo -e "${GREEN}Found libusb version: $libusb_version${NC}"

# Create build directory
BUILD_DIR="${PROJECT_DIR}/build"
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
    echo "Created build directory: $BUILD_DIR"
fi

# Run CMake
echo ""
echo "Running CMake..."
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
echo ""
echo "Building..."
cmake --build . --config Release

# Output build status
if [ -f "$BUILD_DIR/pinscape-config" ]; then
    echo ""
    echo -e "${GREEN}Build successful!${NC}"
    echo "Executable location: $BUILD_DIR/pinscape-config"
    echo ""
    echo "You can install it with:"
    echo "  sudo cmake --install . --prefix /usr/local"
    echo ""
    echo "Or run it directly from the build directory:"
    echo "  $BUILD_DIR/pinscape-config --help"
else
    echo -e "${RED}Build failed${NC}"
    exit 1
fi
