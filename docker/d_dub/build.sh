#!/bin/bash

cp dub.sdl.template dub.sdl

grep dependency ../../dub.sdl | grep -v "path" >> dub.sdl

podman build -t llmfun/app:latest .
podman build -t dlang2:latest .

#podman run --rm -v .:/opt/workarea dlang/llm_fun:1.0 bash -c dub build
