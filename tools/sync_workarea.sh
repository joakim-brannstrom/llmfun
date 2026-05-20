#!/bin/bash -ex

rm -rf llm_fun/workarea/llm_fun/.git
rsync -va --delete source/ llm_fun/workarea/llm_fun/source/
rsync -a  dub.sdl llm_fun/workarea/llm_fun/dub.sdl
rsync -a linenoise.mak llm_fun/workarea/llm_fun/linenoise.mak
rsync -a --delete --exclude ".git" vendor/ llm_fun/workarea/llm_fun/vendor/

pushd llm_fun/workarea/llm_fun
git init
git add source dub.sdl *.mak vendor
git commit -m "import source"
popd
