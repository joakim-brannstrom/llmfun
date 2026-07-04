#!/bin/bash -ex

rm -rf llmfun/workarea/llmfun/.git
rsync -va --delete source/ llmfun/workarea/llmfun/source/
rsync -va --delete cpp_tui/ llmfun/workarea/llmfun/cpp_tui/
rsync -va --delete doc/ llmfun/workarea/llmfun/doc/
rsync -a dub.sdl llmfun/workarea/llmfun/dub.sdl
rsync -a README.md llmfun/workarea/llmfun/
rsync -a config/ llmfun/workarea/llmfun/config/
rsync -a *.mak llmfun/workarea/llmfun/
rsync -a --delete --exclude ".git" vendor/ llmfun/workarea/llmfun/vendor/

pushd llmfun/workarea/llmfun
git init
git add source dub.sdl *.mak vendor
git commit -m "import source"
popd

llmfun rag --db llmfun/data/rag.sqlite3 --sync source/ -i '.*\.d$'
llmfun rag --db llmfun/data/rag.sqlite3 --sync cpp_tui/ -i '.*\.(h|hpp|c|cpp)$'
llmfun rag --db llmfun/data/rag.sqlite3 --sync vendor/imtui/include vendor/imtui/examples -i '.*\.(h|hpp|c|cpp)$'
llmfun rag --db llmfun/data/rag.sqlite3 --sync vendor/imtui/third-party/imgui -i '.*\.(h|hpp|c|cpp)$' -e '.*backend.*' -e '.*examples/libs.*'
