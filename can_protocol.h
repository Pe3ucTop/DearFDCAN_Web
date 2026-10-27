#pragma once
#include <cstdint>
#include <vector>

// gs_usb frame layout based on standard Candlelight specifications
#pragma pack(push, 1)
struct GsHostFrame {
    uint32_t echo_id;
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[64];    // Max payload for CAN FD
};
#pragma pack(pop)

// Structure representing a parsed CAN message ready for the GUI
struct CanMessage {
    uint32_t id;
    bool is_extended;
    bool is_fd;
    uint8_t dlc;
    std::vector<uint8_t> payload;
};

// Protocol handler class managing serialization and deserialization
class CanProtocolHandler {
public:
    // Parse raw USB byte stream into structured CAN messages
    std::vector<CanMessage> ParseRxBuffer(const uint8_t* buffer, int length);

    // Build a raw USB payload from a structured CAN message for transmission
    std::vector<uint8_t> BuildTxBuffer(const CanMessage& msg);

    // Get byte length based on DLC value (Data Length Code)
    static uint8_t DlcToBytes(uint8_t dlc);
    
    // Get DLC value based on byte length
    static uint8_t BytesToDlc(uint8_t bytes);
};