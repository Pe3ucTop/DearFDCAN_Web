#!/bin/sh
#

IMGUI_DIR="../DearImGUI/imgui-1.92.7"
# LIBUSB_DIR="../libusb/out"

em++ WebUSB_Esmcripten_ImGUI__3.cpp \
  ./$IMGUI_DIR/imgui.cpp \
  ./$IMGUI_DIR/imgui_draw.cpp \
  ./$IMGUI_DIR/imgui_tables.cpp \
  ./$IMGUI_DIR/imgui_widgets.cpp \
  ./$IMGUI_DIR/backends/imgui_impl_sdl2.cpp \
  ./$IMGUI_DIR/backends/imgui_impl_opengl3.cpp \
  -I./$IMGUI_DIR \
  -I./$IMGUI_DIR/backends \
  -sUSE_SDL=2 \
  -sMIN_WEBGL_VERSION=2 \
  -sMAX_WEBGL_VERSION=2 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS='["stringToUTF8","lengthBytesUTF8"]' \
  -sEXPORTED_FUNCTIONS='["_main","_malloc","_free"]' \
  -o index.html

# -sEXPORTED_FUNCTIONS='["_main","_malloc","_free","_on_webusb_connected","_on_webusb_disconnected","_on_webusb_error","_on_webusb_tx_complete","_on_webusb_rx_complete"]' \
#  --bind \
# --pre-js redef_Atomics_waitAsync.js \
# -pthread
# -sSHARED_MEMORY=1
#
# -sSINGLE_FILE \

