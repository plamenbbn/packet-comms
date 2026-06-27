#pragma once

#include <string>
#include <vector>

namespace packet_comms {

enum class TransportKind {
    Bluetooth = 1,
    Wifi = 2,
    Lan = 3,
};

struct RemoteDevice {
    TransportKind transport;
    std::string interfaceName;
    std::string macAddress;
    std::string displayName;
    std::string ipAddress;
    int signalStrength = 0;
};

std::vector<RemoteDevice> discover_remote_devices(TransportKind transport);
std::string transport_name(TransportKind transport);
std::string render_remote_device(const RemoteDevice &device);
bool transport_available(TransportKind transport);
std::string local_interface_mac(const std::string &interfaceName);
std::string default_bluetooth_adapter_mac();
std::string interface_name_from_index(unsigned int interfaceIndex);

} // namespace packet_comms
