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
import time
import urllib.request
import urllib.error

url = sys.argv[1]
output = pathlib.Path(sys.argv[2])

last_error = None
for attempt in range(5):
    try:
        with urllib.request.urlopen(url) as response, output.open("wb") as destination:
            shutil.copyfileobj(response, destination)
        last_error = None
        break
    except (urllib.error.HTTPError, urllib.error.URLError) as err:
        last_error = err
        if attempt == 4:
            raise
        time.sleep(2 ** attempt)

if last_error is not None:
    raise last_error
PY
    mv "$REPO_FOLDER/$file.tmp" "$REPO_FOLDER/$file"
  fi
done
