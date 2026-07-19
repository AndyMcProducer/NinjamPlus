#!/bin/sh -x

# Simple script to locally build Standalone, VST3, LV2 on Linux

# Install package deps
sudo apt-get update
sudo apt-get install -y \
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

# Clean out ./build folder
rm -rf build/*

# Initialize ninjam submodule
git submodule update --init --recursive

# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build Standalone, VST3, LV2 targets
cmake --build build --config Release --target "NINJAM_VST3_Standalone;NINJAM_VST3_LV2;NINJAM_VST3_VST3"
