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
rm -rf llmfun/workarea/llmfun/vendor/imtui/test/build

pushd llmfun/workarea/llmfun
git init
git add source dub.sdl *.mak vendor
git commit -m "import source"
popd

llmfun rag --db llmfun/data/rag.sqlite3 --sync -i '.*\.d$' source/
llmfun rag --db llmfun/data/rag.sqlite3 --sync -i '.*\.(h|hpp|c|cpp)$' cpp_tui/
llmfun rag --db llmfun/data/rag.sqlite3 --sync -i '.*\.(h|hpp|c|cpp|md|py)$' -e '.*imtui/test/build/.*' vendor/imtui/test vendor/imtui/include vendor/imtui/examples vendor/imgui_markdown/imgui_markdown.h vendor/imgui_markdown/README.md
llmfun rag --db llmfun/data/rag.sqlite3 --sync -i '.*\.(h|hpp|c|cpp|md|py)$' -e '.*backend.*' -e '.*examples/libs.*' vendor/imtui/third-party/imgui
