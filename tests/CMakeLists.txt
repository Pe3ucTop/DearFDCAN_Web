# Native tests (g++ / clang, AddressSanitizer and UBSan by default). They need no adapter and no browser.
#   test_elmue          the ElmueSoft message / blob parser and builder, error decoding, a fuzzer; the core with transmit records, echoes, events
#   test_clock_sync     the adapter-clock to page-clock fit against a simulated clock (drift, noisy round trips, a 32 bit wrap)
#   test_pcan_records   PEAK error / status / bus load / calibration records, start and stop options, calibration statistics in the core
#   test_app_ui         the adapter drivers driven through the user interface (needs the Dear ImGui sources in IMGUI_DIR)
#   test_webusb_io      libs/webusb-io (UsbPort) against a mock USBDevice       (Node)
#   test_webserial_io   libs/webserial-io (SerialConnection) against a mock SerialPort   (Node)
#   test_serial_worker  the real serial_worker.js (messages of the page and of the CAN worker) over the library and a mock SerialPort   (Node)
# The tests of the workers run under Node against the Emscripten build: tests/test_workers.js (`make test` runs everything).

foreach(name test_elmue test_clock_sync test_pcan_records)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE can_engine can_protocols dearfdcan_sanitize dearfdcan_warnings)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# The JavaScript transport libraries against mock devices (Node, no build needed)
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    foreach(name test_webusb_io test_webserial_io test_serial_worker)
        add_test(NAME ${name} COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.js)
    endforeach()
else()
    message(STATUS "node not found: the tests of webusb-io and webserial-io are not registered")
endif()

if(EXISTS ${IMGUI_DIR}/imgui.cpp)
    # Dear ImGui without the sanitizers (they make it very slow)
    add_library(imgui_native OBJECT
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
    target_include_directories(imgui_native PUBLIC ${IMGUI_DIR})
    target_compile_options(imgui_native PRIVATE -O1)

    # app_ui.cpp compiles natively against stand-ins for <emscripten.h> and <GLFW/glfw3.h> (tests/stub)
    add_executable(test_app_ui
        test_app_ui.cpp ${CMAKE_SOURCE_DIR}/apps/dearfdcan_web/app_ui.cpp $<TARGET_OBJECTS:imgui_native>)
    # (can-engine: only the header can_wire.h, which the bridge header includes; the engine itself is not part of this program)
    target_include_directories(test_app_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stub ${CMAKE_SOURCE_DIR}/apps/dearfdcan_web ${IMGUI_DIR}
                                                   $<TARGET_PROPERTY:can_engine,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(test_app_ui PRIVATE can_adapters dearfdcan_sanitize)
    add_test(NAME test_app_ui COMMAND test_app_ui)
else()
    message(STATUS "Dear ImGui not found in IMGUI_DIR (${IMGUI_DIR}): test_app_ui is not built")
endif()
