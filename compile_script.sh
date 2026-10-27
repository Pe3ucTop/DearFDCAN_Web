#!/bin/sh
#
# Usage: ./compile_script.sh [single|multi]      (default: single)
#
# Thin wrapper around the Makefile (the build logic lives there): same as `make -jN MODE=<mode>`.
# The result goes to build/<mode>/ (single: build/single/index.html is the only file to deploy, and it is
# also copied to docs/index.html, the GitHub Pages folder;
# multi: index.html index.js index.wasm usb_worker.js can_core.js can_core.wasm together).
#
# Requires make, python3, the Emscripten SDK (set up by ./emsdk-exec.sh) and Dear ImGui sources
# (IMGUI_DIR, default /Src_Libs/imgui; override with `IMGUI_DIR=... ./compile_script.sh`).
# Other make variables (OPT, BUILD, ...) can be passed the same way, see `make help`.

set -e

cd "$(dirname "$0")"

MODE="${1:-single}"
case "$MODE" in
  single|multi) ;;
  *) echo "usage: $0 [single|multi]" >&2; exit 2 ;;
esac

if ! command -v make >/dev/null 2>&1; then
  echo "error: make is required (apt install make)" >&2
  exit 1
fi

exec make -j"$(nproc 2>/dev/null || echo 2)" MODE="$MODE"
