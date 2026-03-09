#!/bin/bash

set -euo pipefail

REPO_URL="https://github.com/ps2-mmce/mmceman/releases/download/latest"
REPO_FOLDER="thirdparty/mmce"
FILES=("mmceman.irx" "mmcedrv.irx" "mmceigr.irx")

mkdir -p "$REPO_FOLDER"

for file in "${FILES[@]}"; do
  if test ! -f "$REPO_FOLDER/$file"; then
    python3 - "$REPO_URL/$file" "$REPO_FOLDER/$file.tmp" <<'PY'
import pathlib
import shutil
import sys
import urllib.request

url = sys.argv[1]
output = pathlib.Path(sys.argv[2])

with urllib.request.urlopen(url) as response, output.open("wb") as destination:
    shutil.copyfileobj(response, destination)
PY
    mv "$REPO_FOLDER/$file.tmp" "$REPO_FOLDER/$file"
  fi
done
