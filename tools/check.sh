#!/bin/bash
set -e

# Static Analysis and Formatting Check

if [ -z "$PS2SDK" ]; then
    echo "Warning: PS2SDK environment variable is not set. Skipping format-check."
    exit 0
fi

# Formatting Check
echo "Running formatting check..."
if make format-check; then
    echo "Formatting check passed."
else
    echo "Formatting check failed."
    exit 1
fi
