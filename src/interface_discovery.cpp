#include "interface_discovery.h"

#include "bluetooth_scan.h"

#include <arpa/inet.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace packet_comms {
namespace {

struct ArpEntry {
    std::string ipAddress;
    std::string macAddress;
    std::string interfaceName;
};

std::string trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string normalize_mac(const std::string &value) {
    std::string output;
    output.reserve(value.size());
    for (unsigned char ch : value) {
        output.push_back(static_cast<char>(std::toupper(ch)));
    }
    return output;
}

bool path_exists(const std::filesystem::path &path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool is_wireless_interface(const std::string &interfaceName) {
    const auto wirelessPath = std::filesystem::path("/sys/class/net") / interfaceName / "wireless";
    return path_exists(wirelessPath);
}

bool is_usable_network_interface(const std::string &interfaceName) {
    if (interfaceName == "lo") {
        return false;
    }
    const auto operstatePath = std::filesystem::path("/sys/class/net") / interfaceName / "operstate";
    std::ifstream input(operstatePath);
    std::string operstate;
    if (input && std::getline(input, operstate)) {
        operstate = trim(operstate);
        if (operstate == "down") {
            return false;
        }
    }
    return true;
}

std::vector<std::string> list_interfaces(bool wantWireless) {
    std::vector<std::string> interfaces;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/net")) {
        const std::string interfaceName = entry.path().filename().string();
        if (!is_usable_network_interface(interfaceName)) {
            continue;
        }
        if (is_wireless_interface(interfaceName) != wantWireless) {
            continue;
        }
        interfaces.push_back(interfaceName);
    }
    std::sort(interfaces.begin(), interfaces.end());
    return interfaces;
}

std::vector<ArpEntry> read_arp_entries() {
    std::ifstream input("/proc/net/arp");
    if (!input) {
        throw std::runtime_error("Failed to read /proc/net/arp");
    }

    std::vector<ArpEntry> entries;
    std::string line;
    std::getline(input, line);

    while (std::getline(input, line)) {
        std::istringstream parser(line);
        ArpEntry entry;
        std::string hwType;
        std::string flags;
        std::string mask;
        if (!(parser >> entry.ipAddress >> hwType >> flags >> entry.macAddress >> mask >> entry.interfaceName)) {
            continue;
        }
        if (entry.macAddress == "00:00:00:00:00:00") {
            continue;
        }
        entry.macAddress = normalize_mac(entry.macAddress);
        entries.push_back(std::move(entry));
    }

    return entries;
}

std::optional<std::string> reverse_lookup_name(const std::string &ipAddress) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, ipAddress.c_str(), &address.sin_addr) != 1) {
        return std::nullopt;
    }

    char host[NI_MAXHOST];
    const int result = getnameinfo(reinterpret_cast<sockaddr *>(&address),
                                   sizeof(address),
                                   host,
                                   sizeof(host),
                                   nullptr,
                                   0,
                                   NI_NAMEREQD);
    if (result != 0) {
        return std::nullopt;
    }

    return std::string(host);
}

std::vector<RemoteDevice> discover_udp_candidates(TransportKind transport) {
    const bool wantWireless = transport == TransportKind::Wifi;
    const auto interfaces = list_interfaces(wantWireless);
    if (interfaces.empty()) {
        return {};
    }

    const auto arpEntries = read_arp_entries();
    const std::unordered_set<std::string> interfaceSet(interfaces.begin(), interfaces.end());
    std::set<std::pair<std::string, std::string>> seen;
    std::vector<RemoteDevice> devices;

    for (const auto &entry : arpEntries) {
        if (interfaceSet.find(entry.interfaceName) == interfaceSet.end()) {
            continue;
        }

        const auto seenKey = std::make_pair(entry.interfaceName, entry.macAddress);
        if (!seen.insert(seenKey).second) {
            continue;
        }

        RemoteDevice device;
        device.transport = transport;
        device.interfaceName = entry.interfaceName;
        device.macAddress = entry.macAddress;
        device.ipAddress = entry.ipAddress;
        device.displayName = reverse_lookup_name(entry.ipAddress).value_or(entry.ipAddress);
        devices.push_back(std::move(device));
    }

    std::sort(devices.begin(), devices.end(), [](const RemoteDevice &left, const RemoteDevice &right) {
        if (left.interfaceName != right.interfaceName) {
            return left.interfaceName < right.interfaceName;
        }
        return left.macAddress < right.macAddress;
    });
    return devices;
}

std::vector<RemoteDevice> discover_bluetooth_candidates() {
    std::vector<RemoteDevice> devices;
    for (const auto &bluetoothDevice : discover_bluetooth_devices(5)) {
        RemoteDevice device;
        device.transport = TransportKind::Bluetooth;
        device.interfaceName = "hci0";
        device.macAddress = bluetoothDevice.macAddress;
        device.displayName = bluetoothDevice.name.empty() ? "<unknown>" : bluetoothDevice.name;
        device.signalStrength = bluetoothDevice.rssi;
        devices.push_back(std::move(device));
    }
    return devices;
}

} // namespace

std::vector<RemoteDevice> discover_remote_devices(TransportKind transport) {
    switch (transport) {
    case TransportKind::Bluetooth:
        return discover_bluetooth_candidates();
    case TransportKind::Wifi:
    case TransportKind::Lan:
        return discover_udp_candidates(transport);
    }
    return {};
}

std::string transport_name(TransportKind transport) {
    switch (transport) {
    case TransportKind::Bluetooth:
        return "Bluetooth";
    case TransportKind::Wifi:
        return "Wi-Fi";
    case TransportKind::Lan:
        return "Ethernet";
    }
    return "Unknown";
}

std::string render_remote_device(const RemoteDevice &device) {
    std::ostringstream output;
    output << device.macAddress;
    if (!device.displayName.empty()) {
        output << " | " << device.displayName;
    }
    if (!device.ipAddress.empty()) {
        output << " | " << device.ipAddress;
    }
    if (!device.interfaceName.empty()) {
        output << " | " << device.interfaceName;
    }
    if (device.transport == TransportKind::Bluetooth) {
        output << " | RSSI " << device.signalStrength << " dBm";
    }
    return output.str();
}

bool transport_available(TransportKind transport) {
    switch (transport) {
    case TransportKind::Bluetooth:
        return bluetooth_adapter_available();
    case TransportKind::Wifi:
        return !list_interfaces(true).empty();
    case TransportKind::Lan:
        return !list_interfaces(false).empty();
    }
    return false;
}

std::string local_interface_mac(const std::string &interfaceName) {
    if (interfaceName.empty()) {
        return "";
    }

    const auto addressPath = std::filesystem::path("/sys/class/net") / interfaceName / "address";
    std::ifstream input(addressPath);
    std::string macAddress;
    if (!input || !std::getline(input, macAddress)) {
        return "";
    }
    return normalize_mac(trim(macAddress));
}

std::string default_bluetooth_adapter_mac() {
    const int deviceId = hci_get_route(nullptr);
    if (deviceId < 0) {
        return "";
    }

    bdaddr_t address{};
    if (hci_devba(deviceId, &address) != 0) {
        return "";
    }

    char buffer[18] = {0};
    ba2str(&address, buffer);
    return normalize_mac(buffer);
}

std::string interface_name_from_index(unsigned int interfaceIndex) {
    if (interfaceIndex == 0) {
        return "";
    }

    char nameBuffer[IF_NAMESIZE] = {0};
    if (if_indextoname(interfaceIndex, nameBuffer) == nullptr) {
        return "";
    }
    return std::string(nameBuffer);
}

} // namespace packet_comms
