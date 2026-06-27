# packet-comms

Linux-first C++ packet communication test app for three transport families:

- Bluetooth via BlueZ L2CAP packet sockets
- Wi-Fi via UDP after resolving a selected MAC to an IPv4 neighbor
- Ethernet via UDP after resolving a selected MAC to an IPv4 neighbor

The client is interactive and sends a single `Hello Pi World` packet, then waits
for a response. The server listens on UDP and Bluetooth, prints the received
packet contents as plain text or binary, and replies with:

`Hello Back from <server-interface-mac>`

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

Start the server on the remote machine:

```bash
./build/packet-serveer
```

Run the interactive client:

```bash
./build/packet-client
```

You can also choose a specific server transport:

```bash
./build/packet-serveer --transport udp
./build/packet-serveer --transport wifi
./build/packet-serveer --transport ethernet
./build/packet-serveer --transport bluetooth
```

## Notes

- Bluetooth requires a working BlueZ stack and usually elevated capabilities for
  device discovery.
- Wi-Fi and LAN discovery read `/proc/net/arp`, so the remote peer must already
  be present in the local ARP cache.
- The Wi-Fi and LAN transports use UDP port `37020`.
- The Bluetooth transport uses L2CAP PSM `0x1001`.
- The server prints each received packet with a byte count and renders the
  contents as clear text when printable, otherwise as binary bits prefixed with
  `0b`.
