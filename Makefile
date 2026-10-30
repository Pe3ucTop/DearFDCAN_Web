# Front-end for the CMake build of DearFDCAN_Web (the description of the build is in CMakeLists.txt and the CMakeLists.txt of the libraries).
#
#   make                     single-file build  -> build/single/index.html, copied to docs/index.html
#                                                  (docs/ is the GitHub Pages folder: commit it and publish it)
#   make MODE=multi         multi-file build   -> build/multi/index.html index.js index.wasm
#                                                  usb_worker.js serial_worker.js can_core.js can_core.wasm
#   make test                native unit tests and the Node tests of the transport libraries (build/native, sanitizers) and, if
#                            build/multi / build/single exist, the tests of the workers (multi-file and packed) under Node
#   make serve               serve the result of the selected MODE on http://localhost:8000/
#   make clean               remove the whole build directory
#   make help                this summary
#
# Variables (command line or environment):
#   MODE        single | multi                    (default: single)
#   BUILD       build directory                   (default: build)
#   DOCS        GitHub Pages directory            (default: docs; the single build is published there)
#   IMGUI_DIR   Dear ImGui source directory       (default: /Src_Libs/imgui)
#   BUILD_TYPE  Release (-O2) | Debug (-O0 -g)    (default: Release)
#   EMSDK_EXEC  wrapper that sets up the SDK      (default: ./emsdk-exec.sh)
#   PORT        port of `make serve`              (default: 8000)
#
# Both modes come from one CMake build tree (build/em), so Dear ImGui and the other objects are compiled once; the results are in
# build/single/ and build/multi/. Without make the same is: emcmake cmake -S . -B build/em -DDEARFDCAN_OUT=$PWD/build, then
# cmake --build build/em --target single (or multi). Use `make -jN` or leave the default of one job per core.

MODE       ?= single
BUILD      ?= build
DOCS       ?= docs
IMGUI_DIR  ?= /Src_Libs/imgui
BUILD_TYPE ?= Release
EMSDK_EXEC ?= $(CURDIR)/emsdk-exec.sh
PORT       ?= 8000
JOBS       ?= $(shell nproc 2>/dev/null || echo 2)

ifneq ($(filter $(MODE),single multi),$(MODE))
$(error MODE must be 'single' or 'multi' (got '$(MODE)'))
endif

ABS_BUILD := $(abspath $(BUILD))
ABS_DOCS  := $(abspath $(DOCS))

.PHONY: all single multi configure test serve clean help

all: configure
	$(EMSDK_EXEC) cmake --build $(ABS_BUILD)/em --target $(MODE) -j$(JOBS)
	@echo "Built $(MODE) mode in $(BUILD)/$(MODE)/:"
	@ls -l $(BUILD)/$(MODE) | sed 's/^/  /'

single multi:
	@$(MAKE) --no-print-directory MODE=$@ all

configure:
	$(EMSDK_EXEC) emcmake cmake -S . -B $(ABS_BUILD)/em -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DIMGUI_DIR=$(IMGUI_DIR) \
	    -DDEARFDCAN_OUT=$(ABS_BUILD) -DDEARFDCAN_DOCS_DIR=$(ABS_DOCS)

# Native tests of the libraries and of the adapter drivers; the worker tests need the multi-file build
test:
	cmake -S . -B $(ABS_BUILD)/native -DCMAKE_BUILD_TYPE=Debug -DIMGUI_DIR=$(IMGUI_DIR)
	cmake --build $(ABS_BUILD)/native -j$(JOBS)
	ctest --test-dir $(ABS_BUILD)/native --output-on-failure
	@if [ -f $(BUILD)/multi/can_core.wasm ]; then node tests/test_workers.js $(BUILD)/multi apps/dearfdcan_web; \
	 else echo "(worker tests skipped: run 'make MODE=multi' first)"; fi
	@if [ -f $(BUILD)/tmp/single/usb_worker_bundle.js ]; then \
	   node tests/test_workers.js $(BUILD)/multi apps/dearfdcan_web $(BUILD)/tmp/single/usb_worker_bundle.js && \
	   node tests/test_serial_worker.js apps/dearfdcan_web libs/webserial-io $(BUILD)/tmp/single/usb_worker_bundle.js; \
	 else echo "(packed worker tests skipped: run 'make' first)"; fi

serve:
	@echo "Serving $(BUILD)/$(MODE) on http://localhost:$(PORT)/ (Ctrl+C to stop)"
	python3 -m http.server $(PORT) --bind 127.0.0.1 --directory $(BUILD)/$(MODE)

clean:
	rm -rf $(BUILD)    # docs/ is left alone: it is the published page

help:
	@sed -n '1,/^MODE /p' $(firstword $(MAKEFILE_LIST)) | sed -n '/^#/p' | sed 's/^# \{0,1\}//'
