# can-engine: the core that processes the received transfers of all adapters: frame parsing (through can-protocols), the adapter-clock to
# page-clock mapping and its statistics, the ID table, the trace queue and the compact snapshots (can_wire.h). It is built as an object
# library: the Emscripten module (apps/dearfdcan_web) and the native tests link the objects directly, so the extern "C" functions are
# always present.
add_library(can_engine OBJECT can_core.cpp)
target_include_directories(can_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(can_engine PUBLIC can_protocols PRIVATE dearfdcan_warnings)
