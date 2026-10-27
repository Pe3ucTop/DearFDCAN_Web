#!/bin/sh
#

IMGUI_DIR="../DearImGUI/imgui-1.92.7"
LIBUSB_DIR="../libusb/out"

em++ WebUSB_Esmcripten_ImGUI__1.cpp \
  ./$IMGUI_DIR/imgui.cpp \
  ./$IMGUI_DIR/imgui_draw.cpp \
  ./$IMGUI_DIR/imgui_tables.cpp \
  ./$IMGUI_DIR/imgui_widgets.cpp \
  ./$IMGUI_DIR/backends/imgui_impl_sdl2.cpp \
  ./$IMGUI_DIR/backends/imgui_impl_opengl3.cpp \
  -I./$IMGUI_DIR \
  -I./$IMGUI_DIR/backends \
  -I./$LIBUSB_DIR/include \
  -L./$LIBUSB_DIR/lib -lusb-1.0 \
  -sUSE_SDL=2 \
  -sMIN_WEBGL_VERSION=2 \
  -sMAX_WEBGL_VERSION=2 \
  -sASYNCIFY=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  --pre-js redef_Atomics_waitAsync.js \
  --bind \
  -o index.html

# -pthread
# -sSHARED_MEMORY=1
#
# -sSINGLE_FILE \

