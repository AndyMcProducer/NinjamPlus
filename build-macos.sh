#!/bin/sh -x

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "${NINJAM_BUILD_DIR:-}" in
  "") BUILD_DIR="${SCRIPT_DIR}/build" ;;
  /*) BUILD_DIR="${NINJAM_BUILD_DIR}" ;;
  *) BUILD_DIR="${SCRIPT_DIR}/${NINJAM_BUILD_DIR}" ;;
esac
case "${BUILD_DIR}" in
  ""|/|"${SCRIPT_DIR}") echo "Refusing unsafe build directory: ${BUILD_DIR}" >&2; exit 1 ;;
esac

# Simple script to locally build Standalone, VST3, AU on macOS

# Clean only this checkout's selected build directory.
cmake -E remove_directory "${BUILD_DIR}"

# Initialize ninjam submodule
git -C "${SCRIPT_DIR}" submodule update --init --recursive

# Configure
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release

# Build Standalone, VST3, AU targets
cmake --build "${BUILD_DIR}" --config Release --target NINJAM_VST3_Standalone NINJAM_VST3_AU NINJAM_VST3_VST3
