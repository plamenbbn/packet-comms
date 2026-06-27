#include "client.h"

#include <exception>
#include <iostream>

int main() {
    try {
        return packet_comms::run_client_interactive();
    } catch (const std::exception &error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
