#!/bin/bash
set -e

# setup_deps.sh: Centralized dependency management

echo "Setting up dependencies..."

# 1. Languages
echo " Fetching languages..."
REPO_URL="https://github.com/ps2homebrew/Open-PS2-Loader-lang"
REPO_FOLDER="lng_src"
BRANCH_NAME="main"
if test ! -d "$REPO_FOLDER"; then
  git clone --depth 1 -b $BRANCH_NAME $REPO_URL "$REPO_FOLDER"
else
  (cd "$REPO_FOLDER" && git fetch origin && git reset --hard "origin/${BRANCH_NAME}" && git checkout "$BRANCH_NAME")
fi

# 2. lwNBD
echo " Fetching lwNBD..."
REPO_URL="https://github.com/bignaux/lwNBD.git"
REPO_FOLDER="modules/network/lwNBD"
COMMIT="9777a10f840679ef89b1ec6a588e2d93803d7c37"
if test ! -d "$REPO_FOLDER"; then
  git clone $REPO_URL "$REPO_FOLDER"
  (cd $REPO_FOLDER && git checkout "$COMMIT")
else
  (cd "$REPO_FOLDER" && git fetch origin && git checkout "$COMMIT")
fi

echo "Dependencies up to date."
