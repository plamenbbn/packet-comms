cmake_minimum_required(VERSION 3.20)

project(packet-comms VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_library(BLUEZ_LIBRARY bluetooth REQUIRED)

add_library(packet_comms_lib
    src/bluetooth_scan.cpp
    src/client.cpp
    src/interface_discovery.cpp
    src/transports.cpp
)

target_include_directories(packet_comms_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(packet_comms_lib PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(packet_comms_lib PRIVATE ${BLUEZ_LIBRARY})

add_executable(packet-client
    src/client_main.cpp
)

target_include_directories(packet-client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(packet-client PRIVATE packet_comms_lib)

add_executable(packet-serveer
    src/server_main.cpp
)

target_include_directories(packet-serveer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(packet-serveer PRIVATE packet_comms_lib)
