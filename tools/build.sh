#!/bin/bash
set -e

# Build Hygiene: One-command build wrapper

# Check for PS2SDK
if [ -z "$PS2SDK" ]; then
    echo "Error: PS2SDK environment variable is not set."
    echo "Please set up the PS2SDK environment."
    # In this environment we might not have PS2SDK, so we warn but maybe don't exit if we want to test the script flow?
    # But for a real build script, it should exit.
    # The prompt says "Audit current build instructions... Produce a one-command build wrapper".
    # I will make it exit, as it can't build without it.
    exit 1
fi

# Function to run make
run_make() {
    local target=$1
    local debug_flag=$2

    echo "Building $target..."
    make clean > /dev/null
    if [ -n "$debug_flag" ]; then
        make DEBUG=1 -j$(nproc) > build_$target.log 2>&1
    else
        make -j$(nproc) > build_$target.log 2>&1
    fi

    if [ $? -ne 0 ]; then
        echo "Error building $target. Check build_$target.log for details."
        exit 1
    fi
}

# Clean and Prepare Artifacts directory
rm -rf artifacts
mkdir -p artifacts

# Build Release
run_make "Release" ""
if [ -f OPNPS2LD.ELF ]; then
    cp OPNPS2LD.ELF artifacts/OPL.elf
    echo "Release build successful: artifacts/OPL.elf"
else
    echo "Error: OPNPS2LD.ELF not found after Release build."
    exit 1
fi

# Build Debug
run_make "Debug" "1"
if [ -f OPNPS2LD.ELF ]; then
    cp OPNPS2LD.ELF artifacts/OPL-dbg.elf
    echo "Debug build successful: artifacts/OPL-dbg.elf"
else
    echo "Error: OPNPS2LD.ELF not found after Debug build."
    exit 1
fi

# Version Stamping verification
VERSION=$(make oplversion)
echo "Built version: $VERSION"

echo "Build complete. Artifacts in ./artifacts/"
