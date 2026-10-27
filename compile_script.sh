#!/bin/sh
#

IMGUI_DIR="/Src_Libs/imgui"

# emsdk-exec.sh sources the Emscripten SDK environment and runs the given command
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
  -sEXPORTED_FUNCTIONS='["_main","_malloc","_free","_webusb_cb_connected","_webusb_cb_disconnected","_webusb_cb_snapshot","_webusb_cb_ctrl_in","_webusb_cb_ctrl_ack","_webusb_cb_stats"]' \
  -o index.html

# CAN core for the USB worker: a second, independent wasm module (frame parsing, ID table, trace
# queue, snapshots). It is loaded by usb_worker.js via importScripts('can_core.js').
./emsdk-exec.sh em++ can_core.cpp can_protocol.cpp -O2 -std=c++17 \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=createCanCore \
  -sENVIRONMENT=worker \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS='["HEAPU8"]' \
  -sEXPORTED_FUNCTIONS='["_core_stage_ptr","_core_stage_cap","_core_out_ptr","_core_out_cap","_core_process","_core_publish","_core_clear_table","_core_clear_trace","_core_set_trace_enabled","_core_reset_clock"]' \
  -o can_core.js

# Deploy index.html + index.js + index.wasm + usb_worker.js + can_core.js + can_core.wasm in one
# directory served over HTTP(S). USB I/O runs in a dedicated Worker; no pthreads / SharedArrayBuffer
# are used, so no COOP/COEP headers are required.


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

