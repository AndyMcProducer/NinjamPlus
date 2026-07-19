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

# Simple script to locally build Standalone, VST3, LV2 on Linux

# Install Debian/Ubuntu package dependencies unless the caller already did so.
if [ "${NINJAM_SKIP_SYSTEM_DEPS:-0}" != "1" ]; then
  if ! command -v apt-get >/dev/null 2>&1; then
    echo "apt-get is unavailable; install the documented Linux dependencies or set NINJAM_SKIP_SYSTEM_DEPS=1." >&2
    exit 1
  fi
  SUDO=""
  if [ "$(id -u)" -ne 0 ]; then
    if ! command -v sudo >/dev/null 2>&1; then
      echo "sudo is required to install Linux dependencies; install them manually or set NINJAM_SKIP_SYSTEM_DEPS=1." >&2
      exit 1
    fi
    SUDO="sudo"
  fi
  ${SUDO} apt-get update
  ${SUDO} apt-get install -y \
    ninja-build \
    libasound2-dev \
    libfreetype6-dev \
    libfontconfig1-dev \
    libx11-dev \
    libxcomposite-dev \
    libxcursor-dev \
    libxext-dev \
    libxinerama-dev \
    libxrandr-dev \
    libxrender-dev \
    libxkbcommon-dev \
    libxkbcommon-x11-dev \
    libx11-xcb-dev \
    libxfixes-dev
fi

# Clean only this checkout's selected build directory.
cmake -E remove_directory "${BUILD_DIR}"

# Initialize ninjam submodule
git -C "${SCRIPT_DIR}" submodule update --init --recursive

# Configure
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release

# Build Standalone, VST3, LV2 targets
cmake --build "${BUILD_DIR}" --config Release --target NINJAM_VST3_Standalone NINJAM_VST3_LV2 NINJAM_VST3_VST3
