# Wifydra-SPI 

A distributed WiFi beacon sniffing system using an ESP32 as a central Controller and multiple ESP32-C5 nodes as Workers, with one C5 node per WiFi channel. The device uses SPI to enable synchronization of time, GPS coordinates, and aggregate WiFi discovery data.

Shout out to [lozaning's Wifydra](https://github.com/lozaning/The_Wifydra) for the inspiration!

## Features
- **Passive:** Workers operate in promiscuous mode on a fixed channel, ensuring no missed beacons due to the channel hopping in an active scan.
- **Simplified flashing:** I used SPI's CS pin assignment as unique identifiers, channels are assigned at runtime by the controller. 
- **Wigle Compatibility:** Data is stored in Wigle-compatible structures with first-seen timestamps and RSSI tracking.

## Parts List
- **Controller:** ESP32-WROOM-32 (NodeMCU-32S style), I chose this for the amount of I/O it has, alternative chips should work
- **Workers:** 3x Seeed Studio ESP32-S3 (Initially), scaling to 15x Seeed Studio ESP32-C5.
- **GPS:** NEO-6M u-blox module, any 5v module should work
- **SD Card:** I used a Pzsmocn microSD module, any 3.3v SPI module should work
- **Buck Converter:** Bucking from 5v to 3.3v for the SD card
- **Resistors:** Every SPI connected device (including the microSD) needs a 10k ohm resistor tying the device's 3.3v line to CS
- **CS Multiplexer:** 74HC154 or MCP23017

## Hardware Wiring

Current prototype diagram: https://app.cirkitdesigner.com/project/823f8a95-0d39-48f3-8a6c-8a6a2daa64cd

### SPI Bus Connections
| Signal | ESP32 (Controller) | ESP32-S3 (Worker x) | SD Card (SPI Mode) |
| :--- | :--- | :--- | :--- |
| **SCLK** | GPIO 18 | GPIO 7 | SCLK |
| **MISO** | GPIO 19 | GPIO 8 | MISO |
| **MOSI** | GPIO 23 | GPIO 9 | MOSI |
| **GND** | GND | GND | GND |

### Chip Select (CS) Connections
| Target | ESP32 (Controller CS Pin) | ESP32-S3 (Worker CS Pin) |
| :--- | :--- | :--- |
| **Worker 1** | GPIO 5 | GPIO 2 |
| **Worker 2** | GPIO 4 | GPIO 2 |
| **Worker 3** | GPIO 32 | GPIO 2 |
| **SD Card** | GPIO 21 | N/A |

### GPS Connections (Controller Only)
| Peripheral | Signal | ESP32 (Controller) | Notes |
| :--- | :--- | :--- | :--- |
| **GPS (UART2)** | TX | GPIO 17 | Connect to GPS RX |
| **GPS (UART2)** | RX | GPIO 16 | Connect to GPS TX |

## Requirements
- ESP-IDF 5.x environment sourced: `. $IDF_PATH/export.sh`

## Flashing 
- **Controller:**
  ```bash
  cd controller
  idf.py build
  idf.py flash
  ```
- **Worker:**
  ```bash
  cd worker
  idf.py build
  idf.py flash
  ```