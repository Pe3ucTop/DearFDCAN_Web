# webusb-io: the WebUSB transport (JavaScript, no build step). The monitor's CAN worker uses it; the single-file build packs it into the
# worker, the multi-file build copies it next to the worker. The tests with a mock USBDevice are in tests/ (test_webusb_io.js).
set(WEBUSB_IO_JS ${CMAKE_CURRENT_SOURCE_DIR}/webusb_io.js CACHE INTERNAL "WebUSB transport library")
