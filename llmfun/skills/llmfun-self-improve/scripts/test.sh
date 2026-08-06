#!/bin/sh
# Run the llmfun test suite. Usage: test.sh [other dub args]
set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

repo=$("$SCRIPT_DIR/find-repo.sh") || exit 1
echo "Testing in $repo"
cd "$repo"
exec dub test "$@"
