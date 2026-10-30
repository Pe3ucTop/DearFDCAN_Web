# webserial-io: the Web Serial transport (JavaScript, no build step). The monitor's serial worker uses it; the single-file build packs it
# into the worker, the multi-file build copies it next to the worker. The tests with a mock SerialPort are in tests/ (test_webserial_io.js).
set(WEBSERIAL_IO_JS ${CMAKE_CURRENT_SOURCE_DIR}/webserial_io.js CACHE INTERNAL "Web Serial transport library")
