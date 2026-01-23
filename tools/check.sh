#!/bin/bash
set -e

# Static Analysis and Formatting Check

if [ -z "$PS2SDK" ]; then
    echo "Warning: PS2SDK environment variable is not set. Skipping format-check."
    exit 0
fi

# Formatting Check (using Makefile target if it exists, otherwise echo placeholder)
echo "Running formatting check..."
if grep -q "format-check:" Makefile; then
    if make format-check; then
        echo "Formatting check passed."
    else
        echo "Formatting check failed."
        exit 1
    fi
else
    echo "Makefile does not have format-check target. Skipping."
fi
