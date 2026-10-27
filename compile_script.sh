#!/bin/sh
#
# Usage: ./compile_scipt_Elmue_2.sh [single|multi]      (default: single)
#
#   single  one self-contained index.html: page wasm and CAN core wasm are embedded
#           (-sSINGLE_FILE=1) and the worker source (can_core glue + usb_worker.js) is packed into
#           the page as a string by pack_worker.py; the page starts it from a Blob URL.
#   multi   index.html + index.js + index.wasm + usb_worker.js + can_core.js + can_core.wasm,
#           to be served from one directory (the worker is loaded as usb_worker.js).
#
# No pthreads / SharedArrayBuffer are used, so no COOP/COEP headers are required in either mode.
# Serve over HTTPS or http://localhost (WebUSB needs a secure context); a Content-Security-Policy,
# if any, must allow worker-src blob: for the single mode.

set -e

MODE="${1:-single}"
IMGUI_DIR="/Src_Libs/imgui"
BUILD_DIR="build_tmp"

case "$MODE" in
  single|multi) ;;
  *) echo "usage: $0 [single|multi]" >&2; exit 2 ;;
esac

CORE_EXPORTS='["_core_stage_ptr","_core_stage_cap","_core_out_ptr","_core_out_cap","_core_process","_core_publish","_core_clear_table","_core_clear_trace","_core_set_trace_enabled","_core_reset_clock"]'
PAGE_EXPORTS='["_main","_malloc","_free","_webusb_cb_connected","_webusb_cb_disconnected","_webusb_cb_snapshot","_webusb_cb_ctrl_in","_webusb_cb_ctrl_ack","_webusb_cb_stats"]'

if [ "$MODE" = "single" ]; then
  mkdir -p "$BUILD_DIR"
  CORE_OUT="$BUILD_DIR/can_core.js"
  SINGLE_FLAG="-sSINGLE_FILE=1"
else
  CORE_OUT="can_core.js"
  SINGLE_FLAG=""
fi

# 1. CAN core: independent wasm module that runs inside the USB worker.
#    emsdk-exec.sh sources the Emscripten SDK environment and runs the given command.
./emsdk-exec.sh em++ can_core.cpp can_protocol.cpp -O2 -std=c++17 \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=createCanCore \
  -sENVIRONMENT=worker \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS='["HEAPU8"]' \
  -sEXPORTED_FUNCTIONS="$CORE_EXPORTS" \
  $SINGLE_FLAG \
  -o "$CORE_OUT"

# 2. Single mode: pack can_core glue + usb_worker.js into a --pre-js defining Module.usbWorkerSource
PRE_JS=""
if [ "$MODE" = "single" ]; then
  python3 pack_worker.py "$CORE_OUT" usb_worker.js "$BUILD_DIR/usb_worker_bundle.js"
  PRE_JS="--pre-js $BUILD_DIR/usb_worker_bundle.js"
fi

# 3. The page (Dear ImGui + WebUSB bridge)
./emsdk-exec.sh em++ main.cpp webusb_bridge.cpp can_protocol.cpp app_ui.cpp \
  $IMGUI_DIR/imgui.cpp \
  $IMGUI_DIR/imgui_draw.cpp \
  $IMGUI_DIR/imgui_tables.cpp \
  $IMGUI_DIR/imgui_widgets.cpp \
  $IMGUI_DIR/backends/imgui_impl_glfw.cpp \
  $IMGUI_DIR/backends/imgui_impl_opengl3.cpp \
  -I$IMGUI_DIR \
  -I$IMGUI_DIR/backends \
  -sUSE_GLFW=3 \
  -sMIN_WEBGL_VERSION=2 \
  -sMAX_WEBGL_VERSION=2 \
  -sEXPORTED_RUNTIME_METHODS='["stringToUTF8","lengthBytesUTF8"]' \
  -sEXPORTED_FUNCTIONS="$PAGE_EXPORTS" \
  $SINGLE_FLAG $PRE_JS \
  -o index.html

if [ "$MODE" = "single" ]; then
  echo "Done: deploy index.html alone."
  for f in index.js index.wasm can_core.js can_core.wasm; do
    if [ -e "$f" ]; then
      echo "note: $f is left over from a multi build and is not used by the single-file index.html"
    fi
  done
else
  echo "Done: deploy index.html index.js index.wasm usb_worker.js can_core.js can_core.wasm together."
fi

# $IMGUI_DIR/backends/imgui_impl_sdl2.cpp \
# -sUSE_SDL=2 \

#emcc main.cpp webusb_bridge.cpp can_protocol.cpp app_ui.cpp \
#  ./imgui/imgui.cpp \
#  ./imgui/imgui_draw.cpp \
#  ./imgui/imgui_tables.cpp \
#  ./imgui/imgui_widgets.cpp \
#  ./imgui/backends/imgui_impl_sdl2.cpp \
#  ./imgui/backends/imgui_impl_opengl3.cpp \
#  -I./imgui \
#  -I./imgui/backends \
#  -sUSE_SDL=2 \
#  -sMIN_WEBGL_VERSION=2 \
#  -sMAX_WEBGL_VERSION=2 \
#  -sALLOW_MEMORY_GROWTH=1 \
#  -sEXPORTED_RUNTIME_METHODS='["stringToUTF8","lengthBytesUTF8"]' \
#  -sEXPORTED_FUNCTIONS='["_main","_malloc","_free","_webusb_cb_connected","_webusb_cb_disconnected","_webusb_cb_rx","_webusb_cb_ctrl_in"]' \
#  -o index.html

#em++ main.cpp webusb_bridge.cpp can_protocol.cpp app_ui.cpp \
#  ./imgui/imgui.cpp \
#  ./imgui/imgui_draw.cpp \
#  ./imgui/imgui_tables.cpp \
#  ./imgui/imgui_widgets.cpp \
#  ./imgui/backends/imgui_impl_sdl2.cpp \
#  ./imgui/backends/imgui_impl_opengl3.cpp \
#  -I./imgui \
#  -I./imgui/backends \

#-sEXPORTED_FUNCTIONS='["_main","_malloc","_free", "_webusb_cb_connected", "_webusb_cb_disconnected", "_webusb_cb_rx"]' \

#em++ WebUSB_Esmcripten_ImGUI__4_Elmue.cpp \
#  $IMGUI_DIR/imgui.cpp \
#  $IMGUI_DIR/imgui_draw.cpp \
#  $IMGUI_DIR/imgui_tables.cpp \
#  $IMGUI_DIR/imgui_widgets.cpp \
#  $IMGUI_DIR/backends/imgui_impl_sdl2.cpp \
#  $IMGUI_DIR/backends/imgui_impl_opengl3.cpp \
#  -I$IMGUI_DIR \
#  -I$IMGUI_DIR/backends \
#  -sUSE_SDL=2 \
#  -sMIN_WEBGL_VERSION=2 \
#  -sMAX_WEBGL_VERSION=2 \
#  -sALLOW_MEMORY_GROWTH=1 \
#  -sEXPORTED_RUNTIME_METHODS='["stringToUTF8","lengthBytesUTF8"]' \
#  -sEXPORTED_FUNCTIONS='["_main","_malloc","_free"]' \
#  -o index.html

# -sEXPORTED_FUNCTIONS='["_main","_malloc","_free","_on_webusb_connected","_on_webusb_disconnected","_on_webusb_error","_on_webusb_tx_complete","_on_webusb_rx_complete"]' \
#  --bind \
# --pre-js redef_Atomics_waitAsync.js \
# -pthread
# -sSHARED_MEMORY=1
#
# -sSINGLE_FILE \

