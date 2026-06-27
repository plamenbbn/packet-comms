#include "client.h"

#include "interface_discovery.h"
#include "transports.h"

#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace packet_comms {
namespace {

constexpr const char *kPayloadText = "Hello Pi World";

int prompt_for_number(const std::string &prompt, int minValue, int maxValue) {
    while (true) {
        std::cout << prompt;
        int value = 0;
        if (std::cin >> value && value >= minValue && value <= maxValue) {
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return value;
        }
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "Invalid selection. Try again.\n";
    }
}

TransportKind prompt_for_transport() {
    std::cout << "Select transport:\n";
    std::cout << "  1) Bluetooth\n";
    std::cout << "  2) Wi-Fi\n";
    std::cout << "  3) Ethernet\n";

    while (true) {
        const int selection = prompt_for_number("Choice: ", 1, 3);
        const auto transport = static_cast<TransportKind>(selection);
        if (!transport_available(transport)) {
            std::cout << transport_name(transport) << " is not available on this machine.\n";
            continue;
        }
        return transport;
    }
}

RemoteDevice prompt_for_remote_device(TransportKind transport) {
    std::cout << "Discovering " << transport_name(transport) << " devices...\n";
    const auto devices = discover_remote_devices(transport);
    if (devices.empty()) {
        throw std::runtime_error("No remote devices discovered for the selected transport");
    }

    std::cout << "Available remote devices:\n";
    for (size_t index = 0; index < devices.size(); ++index) {
        std::cout << "  " << (index + 1) << ") " << render_remote_device(devices[index]) << '\n';
    }

    const int selection = prompt_for_number("Select remote device: ", 1, static_cast<int>(devices.size()));
    return devices[static_cast<size_t>(selection - 1)];
}

} // namespace

int run_client_interactive() {
    const TransportKind transport = prompt_for_transport();
    const RemoteDevice device = prompt_for_remote_device(transport);

    const std::vector<uint8_t> payload(kPayloadText, kPayloadText + std::string(kPayloadText).size());
    std::cout << "Sending packet to " << device.macAddress;
    if (!device.ipAddress.empty()) {
        std::cout << " (" << device.ipAddress << ")";
    }
    std::cout << "...\n";

    const PacketResponse response = send_and_receive(device, payload, kDefaultTimeoutMs);
    if (!response.received) {
        std::cerr << "No response packet received before timeout.\n";
        return 1;
    }

    std::cout << "Received packet: " << format_payload_for_output(response.payload) << '\n';
    return 0;
}

} // namespace packet_comms
