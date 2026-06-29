#include "transports.h"

#include <arpa/inet.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace packet_comms {
namespace {

std::atomic<bool> gStopRequested{false};
constexpr const char *kReplyPrefix = "Hello Back from ";

void handle_signal(int) {
    gStopRequested = true;
}

std::vector<uint8_t> make_reply_payload(const std::string &macAddress) {
    const std::string message = std::string(kReplyPrefix)
                              + (macAddress.empty() ? std::string("<unknown>") : macAddress);
    return std::vector<uint8_t>(message.begin(), message.end());
}

std::string normalize_mac_copy(const std::string &value) {
    std::string output;
    output.reserve(value.size());
    for (unsigned char ch : value) {
        output.push_back(static_cast<char>(std::toupper(ch)));
    }
    return output;
}

void set_receive_timeout(int socketFd, int timeoutMs) {
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    if (setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw std::runtime_error(std::string("Failed to set socket timeout: ") + std::strerror(errno));
    }
}

PacketResponse send_udp_and_receive(const RemoteDevice &device,
                                    const std::vector<uint8_t> &payload,
                                    int timeoutMs) {
    if (device.ipAddress.empty()) {
        throw std::runtime_error("Selected device does not have an IPv4 address");
    }

    const int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) {
        throw std::runtime_error(std::string("Failed to create UDP socket: ") + std::strerror(errno));
    }

    try {
        set_receive_timeout(socketFd, timeoutMs);

        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(kUdpPort);
        if (inet_pton(AF_INET, device.ipAddress.c_str(), &remote.sin_addr) != 1) {
            throw std::runtime_error("Invalid IPv4 address for selected device");
        }

        if (sendto(socketFd,
                   payload.data(),
                   payload.size(),
                   0,
                   reinterpret_cast<sockaddr *>(&remote),
                   sizeof(remote)) < 0) {
            throw std::runtime_error(std::string("Failed to send UDP packet: ") + std::strerror(errno));
        }

        std::vector<uint8_t> buffer(4096);
        const ssize_t received = recvfrom(socketFd, buffer.data(), buffer.size(), 0, nullptr, nullptr);
        close(socketFd);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return {};
            }
            throw std::runtime_error(std::string("Failed to receive UDP response: ") + std::strerror(errno));
        }

        buffer.resize(static_cast<size_t>(received));
        return PacketResponse{buffer, true};
    } catch (...) {
        close(socketFd);
        throw;
    }
}

PacketResponse send_bluetooth_and_receive(const RemoteDevice &device,
                                          const std::vector<uint8_t> &payload,
                                          int timeoutMs) {
    const int socketFd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (socketFd < 0) {
        throw std::runtime_error(std::string("Failed to create Bluetooth socket: ") + std::strerror(errno));
    }

    try {
        set_receive_timeout(socketFd, timeoutMs);

        sockaddr_l2 remote{};
        remote.l2_family = AF_BLUETOOTH;
        remote.l2_psm = htobs(kBluetoothPsm);
        if (str2ba(device.macAddress.c_str(), &remote.l2_bdaddr) != 0) {
            throw std::runtime_error("Invalid Bluetooth MAC address");
        }

        if (connect(socketFd, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) != 0) {
            throw std::runtime_error(std::string("Failed to connect over Bluetooth: ") + std::strerror(errno));
        }

        if (send(socketFd, payload.data(), payload.size(), 0) < 0) {
            throw std::runtime_error(std::string("Failed to send Bluetooth packet: ") + std::strerror(errno));
        }

        std::vector<uint8_t> buffer(4096);
        const ssize_t received = recv(socketFd, buffer.data(), buffer.size(), 0);
        close(socketFd);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return {};
            }
            throw std::runtime_error(std::string("Failed to receive Bluetooth response: ") + std::strerror(errno));
        }

        buffer.resize(static_cast<size_t>(received));
        return PacketResponse{buffer, true};
    } catch (...) {
        close(socketFd);
        throw;
    }
}

bool looks_like_text(const std::vector<uint8_t> &payload) {
    if (payload.empty()) {
        return true;
    }
    for (uint8_t byte : payload) {
        if (byte == 0) {
            return false;
        }
        if (!std::isprint(byte) && !std::isspace(byte)) {
            return false;
        }
    }
    return true;
}

std::string describe_payload(const std::vector<uint8_t> &payload) {
    std::ostringstream output;
    output << payload.size() << " bytes";
    if (looks_like_text(payload)) {
        output << ", text: ";
        output << std::string(payload.begin(), payload.end());
        return output.str();
    }

    output << ", binary: 0b";
    for (uint8_t byte : payload) {
        for (int bit = 7; bit >= 0; --bit) {
            output << ((byte >> bit) & 0x01);
        }
        output << ' ';
    }

    std::string rendered = output.str();
    if (!payload.empty()) {
        rendered.pop_back();
    }
    return rendered;
}

void run_udp_server_loop() {
    const int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0) {
        throw std::runtime_error(std::string("Failed to create UDP server socket: ") + std::strerror(errno));
    }

    int reuse = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int packetInfo = 1;
    setsockopt(socketFd, IPPROTO_IP, IP_PKTINFO, &packetInfo, sizeof(packetInfo));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(kUdpPort);

    if (bind(socketFd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        close(socketFd);
        throw std::runtime_error(std::string("Failed to bind UDP server socket: ") + std::strerror(errno));
    }

    set_receive_timeout(socketFd, 500);
    std::cout << "UDP server listening on port " << kUdpPort << '\n';

    while (!gStopRequested.load()) {
        std::vector<uint8_t> buffer(4096);
        sockaddr_in remote{};
        char control[CMSG_SPACE(sizeof(in_pktinfo))] = {0};

        iovec ioVector{};
        ioVector.iov_base = buffer.data();
        ioVector.iov_len = buffer.size();

        msghdr message{};
        message.msg_name = &remote;
        message.msg_namelen = sizeof(remote);
        message.msg_iov = &ioVector;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);

        const ssize_t received = recvmsg(socketFd, &message, 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            close(socketFd);
            throw std::runtime_error(std::string("UDP server receive failed: ") + std::strerror(errno));
        }

        buffer.resize(static_cast<size_t>(received));
        std::string interfaceName;
        for (cmsghdr *controlMessage = CMSG_FIRSTHDR(&message);
             controlMessage != nullptr;
             controlMessage = CMSG_NXTHDR(&message, controlMessage)) {
            if (controlMessage->cmsg_level == IPPROTO_IP
                && controlMessage->cmsg_type == IP_PKTINFO
                && controlMessage->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
                const auto *packetInfoData =
                    reinterpret_cast<const in_pktinfo *>(CMSG_DATA(controlMessage));
                interfaceName = interface_name_from_index(packetInfoData->ipi_ifindex);
                break;
            }
        }

        char remoteAddress[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &remote.sin_addr, remoteAddress, sizeof(remoteAddress));
        std::cout << "UDP packet on "
                  << (interfaceName.empty() ? "<unknown>" : interfaceName)
                  << " from "
                  << remoteAddress
                  << " payload: "
                  << describe_payload(buffer)
                  << '\n';

        const std::vector<uint8_t> replyPayload = make_reply_payload(local_interface_mac(interfaceName));
        if (sendto(socketFd,
                   replyPayload.data(),
                   replyPayload.size(),
                   0,
                   reinterpret_cast<sockaddr *>(&remote),
                   message.msg_namelen) < 0) {
            close(socketFd);
            throw std::runtime_error(std::string("UDP server send failed: ") + std::strerror(errno));
        }
    }

    close(socketFd);
}

void run_bluetooth_server_loop() {
    const int serverFd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (serverFd < 0) {
        throw std::runtime_error(std::string("Failed to create Bluetooth server socket: ") + std::strerror(errno));
    }

    sockaddr_l2 local{};
    local.l2_family = AF_BLUETOOTH;
    local.l2_psm = htobs(kBluetoothPsm);

    if (bind(serverFd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        close(serverFd);
        throw std::runtime_error(std::string("Failed to bind Bluetooth server socket: ") + std::strerror(errno));
    }

    if (listen(serverFd, 1) != 0) {
        close(serverFd);
        throw std::runtime_error(std::string("Failed to listen on Bluetooth server socket: ") + std::strerror(errno));
    }

    set_receive_timeout(serverFd, 500);
    std::cout << "Bluetooth server listening on L2CAP PSM 0x"
              << std::hex << std::uppercase << kBluetoothPsm << std::dec << '\n';

    while (!gStopRequested.load()) {
        sockaddr_l2 remote{};
        socklen_t remoteLength = sizeof(remote);
        const int clientFd = accept(serverFd, reinterpret_cast<sockaddr *>(&remote), &remoteLength);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            close(serverFd);
            throw std::runtime_error(std::string("Bluetooth accept failed: ") + std::strerror(errno));
        }

        try {
            set_receive_timeout(clientFd, 2000);
            std::vector<uint8_t> buffer(4096);
            const ssize_t received = recv(clientFd, buffer.data(), buffer.size(), 0);
            if (received > 0) {
                buffer.resize(static_cast<size_t>(received));
                sockaddr_l2 local{};
                socklen_t localLength = sizeof(local);
                std::string localMacAddress = default_bluetooth_adapter_mac();
                if (getsockname(clientFd, reinterpret_cast<sockaddr *>(&local), &localLength) == 0) {
                    char localAddress[18] = {0};
                    ba2str(&local.l2_bdaddr, localAddress);
                    localMacAddress = normalize_mac_copy(localAddress);
                }

                char remoteAddress[18] = {0};
                ba2str(&remote.l2_bdaddr, remoteAddress);
                std::cout << "Bluetooth packet from "
                          << remoteAddress
                          << " payload: "
                          << describe_payload(buffer)
                          << '\n';

                const std::vector<uint8_t> replyPayload = make_reply_payload(localMacAddress);
                send(clientFd, replyPayload.data(), replyPayload.size(), 0);
            }
            close(clientFd);
        } catch (...) {
            close(clientFd);
            close(serverFd);
            throw;
        }
    }

    close(serverFd);
}

} // namespace

PacketResponse send_and_receive(const RemoteDevice &device,
                                const std::vector<uint8_t> &payload,
                                int timeoutMs) {
    if (device.transport == TransportKind::Bluetooth) {
        return send_bluetooth_and_receive(device, payload, timeoutMs);
    }
    return send_udp_and_receive(device, payload, timeoutMs);
}

std::string format_payload_for_output(const std::vector<uint8_t> &payload) {
    if (looks_like_text(payload)) {
        return std::string(payload.begin(), payload.end());
    }

    std::ostringstream output;
    output << "0x";
    for (uint8_t byte : payload) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return output.str();
}

int run_server(const std::string &transportFilter) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    const bool runUdp = transportFilter == "all" || transportFilter == "udp";
    const bool runBluetooth = transportFilter == "all" || transportFilter == "bluetooth";
    const bool transportAliasAccepted = transportFilter == "wifi" || transportFilter == "ethernet";

    const bool effectiveRunUdp = runUdp || transportAliasAccepted;

    if (!effectiveRunUdp && !runBluetooth) {
        throw std::runtime_error("Unsupported server transport filter");
    }

    std::thread udpThread;
    std::thread bluetoothThread;
    std::exception_ptr udpError;
    std::exception_ptr bluetoothError;

    if (effectiveRunUdp) {
        udpThread = std::thread([&udpError]() {
            try {
                run_udp_server_loop();
            } catch (...) {
                udpError = std::current_exception();
                gStopRequested = true;
            }
        });
    }

    if (runBluetooth) {
        bluetoothThread = std::thread([&bluetoothError]() {
            try {
                run_bluetooth_server_loop();
            } catch (...) {
                bluetoothError = std::current_exception();
                gStopRequested = true;
            }
        });
    }

    if (udpThread.joinable()) {
        udpThread.join();
    }
    if (bluetoothThread.joinable()) {
        bluetoothThread.join();
    }

    if (udpError) {
        std::rethrow_exception(udpError);
    }
    if (bluetoothError) {
        std::rethrow_exception(bluetoothError);
    }

    return 0;
}

} // namespace packet_comms
