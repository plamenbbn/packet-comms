#pragma once

#include "interface_discovery.h"

#include <cstdint>
#include <string>
#include <vector>

namespace packet_comms {

constexpr uint16_t kUdpPort = 37020;
constexpr uint16_t kBluetoothPsm = 0x1001;
constexpr int kDefaultTimeoutMs = 3000;

struct PacketResponse {
    std::vector<uint8_t> payload;
    bool received = false;
};

PacketResponse send_and_receive(const RemoteDevice &device,
                                const std::vector<uint8_t> &payload,
                                int timeoutMs);

int run_server(const std::string &transportFilter);
std::string format_payload_for_output(const std::vector<uint8_t> &payload);

} // namespace packet_comms
