# can-core: the types every CAN component shares (CanMessage, CanPayload, CanBittiming, DLC helpers, bit-timing calculation).
# Header only, C++17, no platform dependency.
add_library(can_types INTERFACE)
target_include_directories(can_types INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
