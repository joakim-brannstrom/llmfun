#!/bin/sh
# Build the llmfun project. Usage: build.sh [--config=<name>] [other dub args]
set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

repo=$("$SCRIPT_DIR/find-repo.sh") || exit 1
echo "Building in $repo"
cd "$repo"
exec dub build "$@"
