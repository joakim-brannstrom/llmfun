#!/bin/bash -ex

rm -rf llmfun/workarea/llmfun/.git
rsync -va --delete source/ llmfun/workarea/llmfun/source/
rsync -a dub.sdl llmfun/workarea/llmfun/dub.sdl
rsync -a README.md llmfun/workarea/llmfun/
rsync -a config/ llmfun/workarea/llmfun/config/
rsync -a linenoise.mak llmfun/workarea/llmfun/
rsync -a sqlite3.mak llmfun/workarea/llmfun/
rsync -a --delete --exclude ".git" vendor/ llmfun/workarea/llmfun/vendor/

pushd llmfun/workarea/llmfun
git init
git add source dub.sdl *.mak vendor
git commit -m "import source"
popd

llmfun rag --db llmfun/data/rag.sqlite3 --sync source/ -i '.*\.d'
