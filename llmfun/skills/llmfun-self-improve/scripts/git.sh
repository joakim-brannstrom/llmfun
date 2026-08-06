#!/bin/sh
# Run any git command in the llmfun repo. Usage: git.sh <git args...>
set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

repo=$(sh "$SCRIPT_DIR/find-repo.sh") || exit 1
cd "$repo"
exec git "$@"
