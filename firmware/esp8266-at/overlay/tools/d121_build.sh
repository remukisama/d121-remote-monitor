#!/usr/bin/env bash
set -euo pipefail
project_path="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_path"
export IDF_PATH="$project_path/esp-idf"
export IDF_TOOLS_PATH="$project_path/.codex-build/idf-tools"
export ESP_AT_PROJECT_PLATFORM=PLATFORM_ESP8266
export ESP_AT_MODULE_NAME=WROOM-02
export ESP_AT_PROJECT_PATH="$project_path"
export SILENCE=0
export PATH="$project_path/.codex-build/venv/bin:$IDF_TOOLS_PATH/tools/xtensa-lx106-elf/esp-2020r3-49-gd5524c1-8.4.0/xtensa-lx106-elf/bin:$PATH"
patch_file="$project_path/patches/sdk-ff16a35-unicode-ldgen.patch"
if [[ "$(git -C esp-idf rev-parse HEAD)" != ff16a35c362047b47c0b3d29c02ef9755b623447 ]]; then
    echo "Unexpected SDK revision; use the pinned SDK before building" >&2
    exit 1
fi
if ! git -C esp-idf apply --reverse --check --unidiff-zero --ignore-whitespace "$patch_file" 2>/dev/null; then
    git -C esp-idf apply --check --unidiff-zero --ignore-whitespace "$patch_file"
    git -C esp-idf apply --unidiff-zero --ignore-whitespace "$patch_file"
fi
mkdir -p build
python -c 'import json;json.dump({"platform":"PLATFORM_ESP8266","module":"WROOM-02","silence":0},open("build/module_info.json","w"))'
if [[ ! -f build/build.ninja || "${D121_RECONFIGURE:-0}" == 1 ]]; then
    python "$IDF_PATH/tools/idf.py" -DIDF_TARGET=esp8266 reconfigure
fi
ninja -C build -j8
