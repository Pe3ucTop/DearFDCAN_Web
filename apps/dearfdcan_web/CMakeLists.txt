# The monitor (Emscripten only): the Dear ImGui page, the CAN worker module (can-engine as WebAssembly) and the two workers.
#
# Targets
#   single   build/single/index.html   one self-contained page (workers packed into it), also copied to the GitHub Pages folder
#   multi    build/multi/              index.html index.js index.wasm usb_worker.js serial_worker.js webusb_io.js webserial_io.js
#                                      can_core.js can_core.wasm
# Both are built from the same objects, so a build of the second mode only links.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(OUT_SINGLE ${DEARFDCAN_OUT}/single)
set(OUT_MULTI  ${DEARFDCAN_OUT}/multi)
set(TMP_SINGLE ${DEARFDCAN_OUT}/tmp/single)

# --- Dear ImGui (compiled without our warnings) ---
if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
    message(FATAL_ERROR "Dear ImGui sources not found in IMGUI_DIR=${IMGUI_DIR} (pass -DIMGUI_DIR=...)")
endif()
add_library(imgui_objs OBJECT
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_objs PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

# --- the page ---
add_library(page_objs OBJECT main.cpp webusb_bridge.cpp app_ui.cpp benchmark.cpp clipboard.cpp)
target_include_directories(page_objs PUBLIC ${APP_DIR})
target_link_libraries(page_objs PUBLIC can_adapters can_engine imgui_objs PRIVATE dearfdcan_warnings)
# (can_engine: only its header can_wire.h is needed here; its objects are linked into the worker module, not into the page)

set(PAGE_EXPORTS
    _main _malloc _free _webusb_cb_device_added _webusb_cb_device_opened _webusb_cb_device_closed _webusb_cb_message
    _webusb_cb_snapshot _webusb_cb_ctrl_in _webusb_cb_ctrl_ack _webusb_cb_stats _webusb_cb_sync _webusb_cb_events _webusb_cb_log)
list(JOIN PAGE_EXPORTS "," PAGE_EXPORTS_ARG)
set(PAGE_LINK_OPTIONS
    -sUSE_GLFW=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    -sEXPORTED_RUNTIME_METHODS=stringToUTF8,lengthBytesUTF8,UTF8ToString
    -sEXPORTED_FUNCTIONS=${PAGE_EXPORTS_ARG}
    "SHELL:--pre-js ${BROWSER_CLIPBOARD_JS}")       # the clipboard (clipboard.cpp); Emscripten's GLFW has none

# --- the CAN worker module: can-engine + can-protocols as WebAssembly ---
set(CORE_EXPORTS
    _core_stage_ptr _core_stage_cap _core_out_ptr _core_out_cap _core_process _core_publish _core_clear_table _core_clear_trace
    _core_set_trace_enabled _core_reset_clock _core_set_protocol _core_reset_stream _core_set_rx_enabled _core_take_replies
    _core_reply_ptr _core_set_elmue _core_clock_sample _core_clock_stats _core_take_events _core_tx_pending _core_tx_forget)
list(JOIN CORE_EXPORTS "," CORE_EXPORTS_ARG)
set(CORE_LINK_OPTIONS
    -sMODULARIZE=1 -sEXPORT_NAME=createCanCore -sENVIRONMENT=worker -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_RUNTIME_METHODS=HEAPU8
    -sEXPORTED_FUNCTIONS=${CORE_EXPORTS_ARG})

# An executable that is only a link of existing objects
function(dearfdcan_module target output_name suffix out_dir)
    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name} SUFFIX ${suffix} RUNTIME_OUTPUT_DIRECTORY ${out_dir}
                                               LINKER_LANGUAGE CXX)
endfunction()

# ---------------------------------------------------------------------------------------------
# single: the module with the wasm embedded is packed with usb_worker.js into a --pre-js of the page
# ---------------------------------------------------------------------------------------------
dearfdcan_module(core_single can_core .js ${TMP_SINGLE} $<TARGET_OBJECTS:can_engine>)
target_link_libraries(core_single PRIVATE can_protocols)
target_link_options(core_single PRIVATE ${CORE_LINK_OPTIONS} -sSINGLE_FILE=1)

set(BUNDLE ${TMP_SINGLE}/usb_worker_bundle.js)
add_custom_command(OUTPUT ${BUNDLE}
    COMMAND ${Python3_EXECUTABLE} ${APP_DIR}/pack_worker.py $<TARGET_FILE:core_single> ${BUNDLE}
            --usb-worker ${APP_DIR}/usb_worker.js --usb-lib ${WEBUSB_IO_JS}
            --serial-worker ${APP_DIR}/serial_worker.js --serial-lib ${WEBSERIAL_IO_JS}
    DEPENDS core_single ${APP_DIR}/usb_worker.js ${APP_DIR}/serial_worker.js ${WEBUSB_IO_JS} ${WEBSERIAL_IO_JS} ${APP_DIR}/pack_worker.py
    COMMENT "Packing the workers into the page" VERBATIM)
add_custom_target(worker_bundle DEPENDS ${BUNDLE})

dearfdcan_module(page_single index .html ${OUT_SINGLE} $<TARGET_OBJECTS:page_objs> $<TARGET_OBJECTS:imgui_objs>)
target_link_libraries(page_single PRIVATE can_adapters)
target_link_options(page_single PRIVATE ${PAGE_LINK_OPTIONS} -sSINGLE_FILE=1 "SHELL:--pre-js ${BUNDLE}")
set_property(TARGET page_single APPEND PROPERTY LINK_DEPENDS ${BUNDLE} ${BROWSER_CLIPBOARD_JS})
add_dependencies(page_single worker_bundle)

# GitHub Pages: the single-file page is published as docs/index.html
if(DEARFDCAN_PUBLISH_DOCS)
    add_custom_command(OUTPUT ${DEARFDCAN_DOCS_DIR}/index.html ${DEARFDCAN_DOCS_DIR}/.nojekyll
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DEARFDCAN_DOCS_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:page_single> ${DEARFDCAN_DOCS_DIR}/index.html
        COMMAND ${CMAKE_COMMAND} -E touch ${DEARFDCAN_DOCS_DIR}/.nojekyll     # GitHub Pages serves the folder as it is
        DEPENDS page_single COMMENT "Publishing the page to ${DEARFDCAN_DOCS_DIR}" VERBATIM)
    add_custom_target(publish_docs DEPENDS ${DEARFDCAN_DOCS_DIR}/index.html)
    add_custom_target(single DEPENDS page_single publish_docs)
else()
    add_custom_target(single DEPENDS page_single)
endif()

# ---------------------------------------------------------------------------------------------
# multi: the page loads usb_worker.js and serial_worker.js and the module next to itself
# ---------------------------------------------------------------------------------------------
dearfdcan_module(core_multi can_core .js ${OUT_MULTI} $<TARGET_OBJECTS:can_engine>)
target_link_libraries(core_multi PRIVATE can_protocols)
target_link_options(core_multi PRIVATE ${CORE_LINK_OPTIONS})

dearfdcan_module(page_multi index .html ${OUT_MULTI} $<TARGET_OBJECTS:page_objs> $<TARGET_OBJECTS:imgui_objs>)
target_link_libraries(page_multi PRIVATE can_adapters)
target_link_options(page_multi PRIVATE ${PAGE_LINK_OPTIONS})
set_property(TARGET page_multi APPEND PROPERTY LINK_DEPENDS ${BROWSER_CLIPBOARD_JS})

# the workers and the transport libraries they load with importScripts()
foreach(file ${APP_DIR}/usb_worker.js ${APP_DIR}/serial_worker.js ${WEBUSB_IO_JS} ${WEBSERIAL_IO_JS})
    get_filename_component(name ${file} NAME)
    add_custom_command(OUTPUT ${OUT_MULTI}/${name}
        COMMAND ${CMAKE_COMMAND} -E copy ${file} ${OUT_MULTI}/${name}
        DEPENDS ${file} VERBATIM)
    list(APPEND MULTI_WORKERS ${OUT_MULTI}/${name})
endforeach()
add_custom_target(workers_multi DEPENDS ${MULTI_WORKERS})
add_custom_target(multi DEPENDS page_multi core_multi workers_multi)
