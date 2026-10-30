# can-adapters: one driver per adapter type (gs_usb with the Elmue mode, PEAK, SLCAN) over an ITransport interface that the platform
# implements: capabilities, start / stop sequences and their acknowledgements, bit-timing limits, transmit frames, bus reports.
# No user interface and no platform code.
add_library(can_adapters STATIC can_adapter.cpp)
target_include_directories(can_adapters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(can_adapters PUBLIC can_protocols PRIVATE dearfdcan_warnings)
