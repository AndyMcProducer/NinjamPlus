#!/bin/sh -x

# Simple script to locally build Standalone, VST3, AU on *NIX platforms

# Clean out ./build folder
rm -rf build/*

# Initialize ninjam submodule
git submodule update --init --recursive

# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build Standalone, VST3, AU targets
cmake --build build --config Release --target "NINJAM_VST3_Standalone;NINJAM_VST3_AU;NINJAM_VST3_VST3"
