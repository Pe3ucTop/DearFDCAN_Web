# DearFDCAN_Web: libraries for CAN adapters (WebUSB / Web Serial) and the Dear ImGui monitor built on them.
#
# This file is the description of the build; the Makefile in this directory is a thin front-end for it (see `make help`).
#
#   Emscripten (the page and the worker module):
#       emcmake cmake -S . -B build/em -DDEARFDCAN_OUT=$PWD/build && cmake --build build/em --target single   (or: multi)
#   Native (unit tests of the libraries and of the adapter drivers, no adapter and no browser needed):
#       cmake -S . -B build/native && cmake --build build/native && ctest --test-dir build/native
#
# Layout (see DECISIONS.md):
#   libs/can-core           types, DLC and bit-timing helpers                     (header only)
#   libs/can-protocols      gs_usb, ElmueSoft, PEAK PCAN-USB FD and SLCAN: frames, commands, reports
#   libs/can-engine         the worker core: clock sync, ID table, trace, snapshots
#   libs/can-adapters       one driver per adapter over an ITransport interface
#   libs/emscripten-webserial   Web Serial ports as /dev/ttyWebN (stand-alone, has its own README and test)
#   apps/dearfdcan_web      the monitor: windows, the bridge to the workers, the workers (Emscripten only)
#   tests                   native unit tests and the headless test of the user interface
cmake_minimum_required(VERSION 3.20)
project(DearFDCAN_Web LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Release (-O2), Debug (-O0 -g)" FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

set(IMGUI_DIR "/Src_Libs/imgui" CACHE PATH "Dear ImGui sources (imgui.cpp, backends/)")
set(DEARFDCAN_OUT "${CMAKE_BINARY_DIR}" CACHE PATH "Directory that gets single/ and multi/ (the results)")
set(DEARFDCAN_DOCS_DIR "${CMAKE_SOURCE_DIR}/docs" CACHE PATH "GitHub Pages folder: the single-file page is copied there")
option(DEARFDCAN_PUBLISH_DOCS "Copy the single-file page to DEARFDCAN_DOCS_DIR" ON)
option(DEARFDCAN_BUILD_TESTS "Build the native tests (not with Emscripten)" ON)
option(DEARFDCAN_SANITIZE "Native tests: AddressSanitizer and UBSan" ON)

# Warnings for our own sources only (Dear ImGui is built without them)
add_library(dearfdcan_warnings INTERFACE)
target_compile_options(dearfdcan_warnings INTERFACE -Wall -Wextra)

# Native tests run the libraries under the sanitizers; everything that links this target gets them
add_library(dearfdcan_sanitize INTERFACE)
if(NOT EMSCRIPTEN AND DEARFDCAN_SANITIZE)
    target_compile_options(dearfdcan_sanitize INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=undefined -g)
    target_link_options(dearfdcan_sanitize INTERFACE -fsanitize=address,undefined)
endif()

add_subdirectory(libs/can-core)
add_subdirectory(libs/can-protocols)
add_subdirectory(libs/can-engine)
add_subdirectory(libs/can-adapters)

if(EMSCRIPTEN)
    add_subdirectory(apps/dearfdcan_web)
elseif(DEARFDCAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
