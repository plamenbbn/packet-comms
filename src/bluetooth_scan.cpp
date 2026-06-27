#include "bluetooth_scan.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace packet_comms {
namespace {

struct LeAdvertisingHeader {
    uint8_t evtType;
    uint8_t addrType;
    bdaddr_t address;
    uint8_t dataLength;
} __attribute__((packed));

std::string trim_name(const std::string &value) {
    std::string cleaned;
    for (unsigned char ch : value) {
        if (std::isprint(ch)) {
            cleaned.push_back(static_cast<char>(ch));
        }
    }
    return cleaned;
}

std::string parse_advertised_name(const uint8_t *data, uint8_t length) {
    size_t offset = 0;
    std::string fallback;

    while (offset < length) {
        uint8_t fieldLength = data[offset];
        if (fieldLength == 0 || offset + fieldLength + 1 > length) {
            break;
        }

        uint8_t fieldType = data[offset + 1];
        if (fieldType == 0x09 && fieldLength >= 2) {
            return trim_name(
                std::string(reinterpret_cast<const char *>(&data[offset + 2]), fieldLength - 1));
        }
        if (fieldType == 0x08 && fieldLength >= 2 && fallback.empty()) {
            fallback.assign(reinterpret_cast<const char *>(&data[offset + 2]), fieldLength - 1);
        }
        offset += fieldLength + 1;
    }

    return trim_name(fallback);
}

void resolve_missing_names(int socketFd, std::map<std::string, BluetoothDevice> &devices) {
    for (auto &entry : devices) {
        BluetoothDevice &device = entry.second;
        if (!device.name.empty()) {
            continue;
        }

        bdaddr_t address{};
        if (str2ba(device.macAddress.c_str(), &address) != 0) {
            continue;
        }

        char name[248] = {0};
        if (hci_read_remote_name(socketFd, &address, sizeof(name), name, 0) == 0) {
            device.name = trim_name(name);
        }
    }
}

} // namespace

bool bluetooth_adapter_available() {
    return hci_get_route(nullptr) >= 0;
}

std::vector<BluetoothDevice> discover_bluetooth_devices(int scanDurationSeconds) {
    const int deviceId = hci_get_route(nullptr);
    if (deviceId < 0) {
        throw std::runtime_error("No Bluetooth adapter available");
    }

    const int socketFd = hci_open_dev(deviceId);
    if (socketFd < 0) {
        throw std::runtime_error("Failed to open HCI device");
    }

    std::map<std::string, BluetoothDevice> devices;

    const int flags = fcntl(socketFd, F_GETFL);
    if (flags >= 0) {
        fcntl(socketFd, F_SETFL, flags | O_NONBLOCK);
    }

    hci_filter filter;
    hci_filter_clear(&filter);
    hci_filter_set_ptype(HCI_EVENT_PKT, &filter);
    hci_filter_set_event(EVT_LE_META_EVENT, &filter);
    if (setsockopt(socketFd, SOL_HCI, HCI_FILTER, &filter, sizeof(filter)) < 0) {
        close(socketFd);
        throw std::runtime_error("Could not set HCI filter");
    }

    hci_le_set_scan_enable(socketFd, 0x00, 1, 1000);

    if (hci_le_set_scan_parameters(socketFd,
                                   0x01,
                                   htobs(0x0010),
                                   htobs(0x0010),
                                   LE_PUBLIC_ADDRESS,
                                   0x00,
                                   1000) < 0) {
        const int err = errno;
        close(socketFd);
        throw std::runtime_error(std::string("Failed to set scan parameters: ") + std::strerror(err));
    }

    if (hci_le_set_scan_enable(socketFd, 0x01, 1, 1000) < 0) {
        const int err = errno;
        close(socketFd);
        throw std::runtime_error(std::string("Failed to enable scan: ") + std::strerror(err));
    }

    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(scanDurationSeconds);
    unsigned char buffer[HCI_MAX_EVENT_SIZE];

    while (std::chrono::steady_clock::now() < end) {
        const int length = read(socketFd, buffer, sizeof(buffer));
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            break;
        }

        auto *meta = reinterpret_cast<evt_le_meta_event *>(buffer + (1 + HCI_EVENT_HDR_SIZE));
        if (meta->subevent != EVT_LE_ADVERTISING_REPORT) {
            continue;
        }

        uint8_t reportCount = meta->data[0];
        uint8_t *cursor = meta->data + 1;

        for (uint8_t reportIndex = 0; reportIndex < reportCount; ++reportIndex) {
            auto *advertisingInfo = reinterpret_cast<LeAdvertisingHeader *>(cursor);
            const uint8_t *advertisingData = cursor + sizeof(LeAdvertisingHeader);

            char addressBuffer[18];
            ba2str(&advertisingInfo->address, addressBuffer);

            BluetoothDevice &device = devices[addressBuffer];
            device.macAddress = addressBuffer;
            device.name = parse_advertised_name(advertisingData, advertisingInfo->dataLength);
            device.rssi = static_cast<int8_t>(advertisingData[advertisingInfo->dataLength]);

            cursor += sizeof(LeAdvertisingHeader) + advertisingInfo->dataLength + 1;
        }
    }

    hci_le_set_scan_enable(socketFd, 0x00, 1, 1000);
    resolve_missing_names(socketFd, devices);
    close(socketFd);

    std::vector<BluetoothDevice> results;
    results.reserve(devices.size());
    for (auto &entry : devices) {
        results.push_back(std::move(entry.second));
    }
    std::sort(results.begin(), results.end(), [](const BluetoothDevice &left, const BluetoothDevice &right) {
        return left.macAddress < right.macAddress;
    });
    return results;
}

} // namespace packet_comms
