#!/bin/sh
# Builds and runs the headless test of the adapter sequences (tests/test_app_ui.cpp) natively with g++, ASan and UBSan.
# It needs the Dear ImGui sources (IMGUI_DIR, default /Src_Libs/imgui) and GLFW headers (libglfw3-dev), nothing else.
# Output goes to $TMPDIR (or /tmp); the sources are not touched.
set -e
cd "$(dirname "$0")/.."
IMGUI_DIR="${IMGUI_DIR:-/Src_Libs/imgui}"
OUT="${TMPDIR:-/tmp}/dearfdcan_tests"
mkdir -p "$OUT/imgui"
SRC=src_inc

# Dear ImGui once, without sanitizers (they make it very slow)
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  [ -f "$OUT/imgui/$f.o" ] || g++ -std=c++17 -O1 -c "$IMGUI_DIR/$f.cpp" -I"$IMGUI_DIR" -o "$OUT/imgui/$f.o"
done

g++ -std=c++17 -g -O0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra \
    -Itests/stub -I$SRC -I"$IMGUI_DIR" tests/test_app_ui.cpp $SRC/app_ui.cpp $SRC/can_adapter.cpp \
    $SRC/can_protocol.cpp $SRC/can_protocol_gsusb.cpp $SRC/can_protocol_elmue.cpp $SRC/can_protocol_pcan.cpp $SRC/can_protocol_slcan.cpp \
    "$OUT"/imgui/*.o -o "$OUT/test_app_ui"
"$OUT/test_app_ui"
