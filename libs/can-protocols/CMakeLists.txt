# can-protocols: what the adapters speak. One module per protocol (structures, frame parsing and building, command sequences) and the
# CanProtocolHandler facade. No I/O, no platform dependency.
#   gsusb   gs_usb (candleLight / CANable)
#   elmue   the ElmueSoft CANable 2.5 extension of gs_usb
#   pcan    PEAK PCAN-USB FD (uCAN); the record layouts come from PEAK's Linux driver, see DECISIONS.md
#   slcan   SLCAN (Lawicel / CANable slcan firmware)
add_library(can_protocols STATIC
    can_protocol.cpp
    can_protocol_gsusb.cpp
    can_protocol_elmue.cpp
    can_protocol_pcan.cpp
    can_protocol_slcan.cpp)
target_include_directories(can_protocols PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(can_protocols PUBLIC can_types dearfdcan_sanitize PRIVATE dearfdcan_warnings)
