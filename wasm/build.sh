#!/bin/sh
# ブラウザ用の WASM（docs/qm.js, docs/qm.wasm）。EMSDK で emsdk の場所を変えられる
set -e
cd "$(dirname "$0")/.."
EMSDK="${EMSDK:-/c/prog/emsdk/emsdk}"
EMCC="$EMSDK/upstream/emscripten/emcc.exe"
[ -x "$EMCC" ] || EMCC="$EMSDK/upstream/emscripten/emcc"
"$EMCC" -std=c++17 -O3 -msimd128 -DNDEBUG -I. -Ithird_party -Ithird_party/nlohmann \
  -s MODULARIZE=1 -s EXPORT_NAME=createQM -s ALLOW_MEMORY_GROWTH=1 -s STACK_SIZE=4194304 \
  -s EXPORTED_RUNTIME_METHODS='["HEAPU8","UTF8ToString","stringToUTF8","lengthBytesUTF8"]' -s EXPORTED_FUNCTIONS='["_malloc","_free"]' \
  wasm/qm_wasm.cpp -o docs/qm.js
cp data/basemap.txt docs/basemap.txt
cp fonts/NotoSansJP-Bold-jis.ttf docs/font.ttf
echo "built docs/qm.js"
