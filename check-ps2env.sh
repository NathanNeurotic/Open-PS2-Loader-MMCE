#!/usr/bin/env bash
set -euo pipefail

echo "PWD=$PWD"
echo "PS2DEV=${PS2DEV:-}"
echo "PS2SDK=${PS2SDK:-}"
echo "PATH=$PATH"

command -v mips64r5900el-ps2-elf-gcc
command -v mipsel-none-elf-gcc

test -f "$PS2SDK/Defs.make"

echo "PS2 environment is visible and ready."
