# emscripten-webusb: WebUSB for programs compiled with Emscripten (webusb.h + library_webusb.js, on top of libs/webusb-io).
# A program uses it like this (no CMake integration is needed, these are plain link options):
#     em++ app.cpp -Ilibs/emscripten-webusb --pre-js libs/webusb-io/webusb_io.js --js-library libs/emscripten-webusb/library_webusb.js
#
# Here only the test is described (Emscripten configuration): test_webusb.c is linked for Node and run against a mock navigator.usb by
#     cmake --build build/em --target run_test_webusb
if(EMSCRIPTEN)
    set(WEBUSB_LIBRARY_JS ${CMAKE_CURRENT_SOURCE_DIR}/library_webusb.js CACHE INTERNAL "WebUSB library for Emscripten programs")

    add_executable(test_webusb EXCLUDE_FROM_ALL test/test_webusb.c)
    set_target_properties(test_webusb PROPERTIES OUTPUT_NAME test_webusb SUFFIX .js RUNTIME_OUTPUT_DIRECTORY ${DEARFDCAN_OUT}/tmp/test_webusb)
    target_link_options(test_webusb PRIVATE
        -sENVIRONMENT=node -sMODULARIZE=1 -sEXPORT_NAME=createWU -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString
        "SHELL:--pre-js ${WEBUSB_IO_JS}" "SHELL:--js-library ${WEBUSB_LIBRARY_JS}")
    set_property(TARGET test_webusb APPEND PROPERTY LINK_DEPENDS ${WEBUSB_IO_JS} ${WEBUSB_LIBRARY_JS})

    find_program(NODE_EXECUTABLE node)
    if(NODE_EXECUTABLE)
        add_custom_target(run_test_webusb
            COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/run_test.js $<TARGET_FILE:test_webusb>
            DEPENDS test_webusb USES_TERMINAL)
    endif()
endif()
