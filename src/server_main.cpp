#include "transports.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    try {
        std::string transportFilter = "all";
        if (argc == 3 && std::string(argv[1]) == "--transport") {
            transportFilter = argv[2];
        } else if (argc != 1) {
            std::cerr << "Usage: " << argv[0] << " [--transport udp|wifi|ethernet|bluetooth|all]\n";
            return 2;
        }

        return packet_comms::run_server(transportFilter);
    } catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
