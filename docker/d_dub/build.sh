#!/bin/bash

cp ../../dub.sdl .

podman build -t dlang/llmfun:1.0 .

#podman run --rm -v .:/opt/workarea dlang/llm_fun:1.0 bash -c dub build
