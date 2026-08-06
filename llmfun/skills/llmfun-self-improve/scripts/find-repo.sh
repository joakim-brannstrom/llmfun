#!/bin/sh
# Print the absolute path of the llmfun repo (the directory containing dub.sdl).
# Searches upward from this script's location. Honors the $LLMFUN_REPO override.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ -n "$LLMFUN_REPO" ] && [ -f "$LLMFUN_REPO/dub.sdl" ]; then
    echo "$LLMFUN_REPO"
    exit 0
fi

dir=$SCRIPT_DIR
while [ "$dir" != "/" ]; do
    if [ -f "$dir/dub.sdl" ]; then
        echo "$dir"
        exit 0
    fi
    if [ -f "$dir/llmfun/dub.sdl" ]; then
        echo "$dir/llmfun"
        exit 0
    fi
    dir=$(dirname "$dir")
done

echo "llmfun repo not found (searched from $SCRIPT_DIR)" >&2
exit 1
