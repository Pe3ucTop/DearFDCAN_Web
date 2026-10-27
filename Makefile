# Build driver for DearFDCAN_Web (Emscripten).
#
#   make                     single-file build  -> build/single/index.html, copied to docs/index.html
#                                                  (docs/ is the GitHub Pages folder: commit it and publish it)
#   make MODE=multi         multi-file build   -> build/multi/index.html index.js index.wasm
#                                                  usb_worker.js can_core.js can_core.wasm
#   make serve               serve the result of the selected MODE on http://localhost:8000/
#   make clean               remove the whole build directory
#   make help                this summary
#
# Variables (command line or environment):
#   MODE        single | multi                    (default: single)
#   BUILD       build directory                   (default: build)
#   DOCS        GitHub Pages directory            (default: docs; the single build is published there)
#   IMGUI_DIR   Dear ImGui source directory       (default: /Src_Libs/imgui)
#   OPT         optimization flag                 (default: -O2; use -O0 -g for debugging)
#   EMSDK_EXEC  wrapper that sets up the SDK      (default: ./emsdk-exec.sh)
#   PORT        port of `make serve`              (default: 8000)
#
# Compiled objects are cached in $(BUILD)/obj and shared by both modes, so Dear ImGui is compiled only
# once and a change of one source file recompiles just that file. Use `make -jN` to build in parallel.

MODE       ?= single
BUILD      ?= build
DOCS       ?= docs
IMGUI_DIR  ?= /Src_Libs/imgui
OPT        ?= -O2
EMSDK_EXEC ?= $(CURDIR)/emsdk-exec.sh
PORT       ?= 8000

SRC := src_inc
CXX := $(EMSDK_EXEC) em++

ifeq ($(MODE),single)
SINGLE_FLAG := -sSINGLE_FILE=1
else ifeq ($(MODE),multi)
SINGLE_FLAG :=
else
$(error MODE must be 'single' or 'multi' (got '$(MODE)'))
endif

OUT := $(BUILD)/$(MODE)
TMP := $(BUILD)/tmp/$(MODE)
OBJ := $(BUILD)/obj

# ---------------------------------------------------------------------------------------------
# Page: Dear ImGui user interface + WebUSB bridge
# ---------------------------------------------------------------------------------------------
APP_SRCS   := main.cpp webusb_bridge.cpp can_protocol.cpp can_protocol_gsusb.cpp can_protocol_pcan.cpp app_ui.cpp
IMGUI_SRCS := imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp \
              backends/imgui_impl_glfw.cpp backends/imgui_impl_opengl3.cpp

PAGE_OBJS := $(addprefix $(OBJ)/page/,$(APP_SRCS:.cpp=.o)) \
             $(addprefix $(OBJ)/imgui/,$(IMGUI_SRCS:.cpp=.o))

PAGE_COMMON := -std=c++17 $(OPT) -I$(SRC) -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends

PAGE_EXPORTS := ["_main","_malloc","_free","_webusb_cb_device_added","_webusb_cb_device_opened","_webusb_cb_device_closed","_webusb_cb_message","_webusb_cb_snapshot","_webusb_cb_ctrl_in","_webusb_cb_ctrl_ack","_webusb_cb_stats"]
PAGE_LDFLAGS := $(OPT) -sUSE_GLFW=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
                -sEXPORTED_RUNTIME_METHODS='["stringToUTF8","lengthBytesUTF8"]' \
                -sEXPORTED_FUNCTIONS='$(PAGE_EXPORTS)'

# ---------------------------------------------------------------------------------------------
# CAN core: separate wasm module that runs inside the USB worker
# ---------------------------------------------------------------------------------------------
CORE_OBJS := $(OBJ)/core/can_core.o $(OBJ)/core/can_protocol.o $(OBJ)/core/can_protocol_gsusb.o $(OBJ)/core/can_protocol_pcan.o

CORE_COMMON := -std=c++17 $(OPT) -I$(SRC)

CORE_EXPORTS := ["_core_stage_ptr","_core_stage_cap","_core_out_ptr","_core_out_cap","_core_process","_core_publish","_core_clear_table","_core_clear_trace","_core_set_trace_enabled","_core_reset_clock","_core_set_protocol","_core_reset_stream"]
CORE_LDFLAGS := $(OPT) -sMODULARIZE=1 -sEXPORT_NAME=createCanCore -sENVIRONMENT=worker \
                -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_RUNTIME_METHODS='["HEAPU8"]' \
                -sEXPORTED_FUNCTIONS='$(CORE_EXPORTS)'

ifeq ($(MODE),single)
# The core glue (wasm embedded) is only an input of the packed worker, the page carries everything
CORE_JS    := $(TMP)/can_core.js
BUNDLE     := $(TMP)/usb_worker_bundle.js
PRE_JS     := --pre-js $(BUNDLE)
PAGE_DEPS  := $(BUNDLE)
ARTIFACTS  := $(OUT)/index.html $(DOCS)/index.html $(DOCS)/.nojekyll
else
CORE_JS    := $(OUT)/can_core.js
BUNDLE     :=
PRE_JS     :=
PAGE_DEPS  :=
ARTIFACTS  := $(OUT)/index.html $(OUT)/can_core.js $(OUT)/usb_worker.js
endif

# ---------------------------------------------------------------------------------------------
.PHONY: all single multi serve clean help

all: $(ARTIFACTS)
	@echo "Built $(MODE) mode in $(OUT)/:"
	@ls -l $(OUT) | sed 's/^/  /'

single multi:
	@$(MAKE) --no-print-directory MODE=$@ all

# --- page objects ---
$(OBJ)/page/%.o: $(SRC)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(PAGE_COMMON) -Wall -Wextra -MMD -MP

$(OBJ)/imgui/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(PAGE_COMMON) -MMD -MP

# --- core objects ---
$(OBJ)/core/%.o: $(SRC)/%.cpp
	@mkdir -p $(@D)
	$(CXX) -c $< -o $@ $(CORE_COMMON) -Wall -Wextra -MMD -MP

# --- core module (single: wasm embedded in the glue) ---
$(CORE_JS): $(CORE_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CORE_OBJS) -o $@ $(CORE_LDFLAGS) $(SINGLE_FLAG)

# --- single mode: worker source (core glue + usb_worker.js) packed into a --pre-js for the page ---
ifeq ($(MODE),single)
$(BUNDLE): $(CORE_JS) $(SRC)/usb_worker.js pack_worker.py
	@mkdir -p $(@D)
	python3 pack_worker.py $(CORE_JS) $(SRC)/usb_worker.js $@
endif

# --- multi mode: the page loads usb_worker.js next to itself ---
ifeq ($(MODE),multi)
$(OUT)/usb_worker.js: $(SRC)/usb_worker.js
	@mkdir -p $(@D)
	cp $< $@
endif

# --- the page (index.js/index.wasm are side outputs of the same link in multi mode) ---
$(OUT)/index.html: $(PAGE_OBJS) $(PAGE_DEPS)
	@mkdir -p $(@D)
	$(CXX) $(PAGE_OBJS) -o $@ $(PAGE_LDFLAGS) $(SINGLE_FLAG) $(PRE_JS)

# --- GitHub Pages: the single-file page is published as docs/index.html (single mode only) ---
ifeq ($(MODE),single)
$(DOCS)/index.html: $(OUT)/index.html
	@mkdir -p $(@D)
	cp $< $@

# Tells GitHub Pages to serve the folder as is (no Jekyll processing)
$(DOCS)/.nojekyll:
	@mkdir -p $(@D)
	touch $@
endif

serve:
	@echo "Serving $(OUT) on http://localhost:$(PORT)/ (Ctrl+C to stop)"
	python3 -m http.server $(PORT) --bind 127.0.0.1 --directory $(OUT)

clean:
	rm -rf $(BUILD)    # docs/ is left alone: it is the published page

help:
	@sed -n '1,/^MODE /p' $(firstword $(MAKEFILE_LIST)) | sed -n '/^#/p' | sed 's/^# \{0,1\}//'

-include $(PAGE_OBJS:.o=.d) $(CORE_OBJS:.o=.d)
