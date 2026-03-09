#!/bin/bash

set -euo pipefail

REPO_URL="https://github.com/ps2-mmce/mmceman/releases/download/latest"
REPO_FOLDER="thirdparty/mmce"
FILES=("mmceman.irx" "mmcedrv.irx" "mmceigr.irx")

mkdir -p "$REPO_FOLDER"

for file in "${FILES[@]}"; do
  if test ! -f "$REPO_FOLDER/$file"; then
    curl -L --fail --silent --show-error "$REPO_URL/$file" -o "$REPO_FOLDER/$file.tmp" || { exit 1; }
    mv "$REPO_FOLDER/$file.tmp" "$REPO_FOLDER/$file"
  fi
done
