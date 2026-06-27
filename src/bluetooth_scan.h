#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace packet_comms {

struct BluetoothDevice {
    std::string macAddress;
    std::string name;
    int8_t rssi = 0;
};

std::vector<BluetoothDevice> discover_bluetooth_devices(int scanDurationSeconds);
bool bluetooth_adapter_available();

} // namespace packet_comms
