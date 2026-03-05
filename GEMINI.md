# SPI Communication Project: Controller-Worker WiFi Sniffer

This project establishes a high-speed, reliable SPI communication link between an ESP32 Controller and multiple ESP32-S3 Worker nodes using the ESP-IDF framework.

## Project Overview
- **Architecture:** 1 Controller (Master) polling up to 3 Workers (Slaves).
- **Primary Goal:** Distributed WiFi beacon sniffing and MAC address tracking in a Wigle-compatible format.
- **Technologies:** 
    - **Framework:** ESP-IDF (C)
    - **Communication:** SPI at 1MHz (with fallback to 500kHz for signal integrity).
    - **Features:** WiFi Promiscuous mode, DMA-backed SPI transfers, Mutex-protected shared state, Time/GPS synchronization.

## Architecture Details
### SPI Protocol (Fixed-Frame)
- **Frame Size:** Exactly 5100 bytes.
- **Synchronization:** "Continuous Queuing" model on Workers to prevent timing races.
- **Checksum:** 16-bit additive sum over the data payload.
- **Data Structures:** 
    - `controller_msg_t`: Sends command, WiFi channel, unix time, and GPS coordinates.
    - `worker_msg_t`: Returns status, record count, checksum, and an array of `wigle_record_t`.

### WiFi Sniffing (Workers)
- **Mode:** Promiscuous (Monitor) mode.
- **Filter:** Specifically targets Beacon frames (Type 0, Subtype 8).
- **Storage:** Up to 500 unique APs tracked in internal memory.
- **Efficiency:** Uses a "Sent" flag logic to transmit only new discoveries across the SPI bus.

## Hardware Wiring (Standard)
| Signal | Controller (ESP32) | Worker (ESP32-S3) |
| :--- | :--- | :--- |
| **SCLK** | GPIO 18 | GPIO 7 |
| **MISO** | GPIO 19 | GPIO 8 |
| **MOSI** | GPIO 23 | GPIO 9 |
| **CS (Worker 1)** | GPIO 5 | GPIO 2 |
| **CS (Worker 2)** | GPIO 4 | GPIO 2 |
| **CS (Worker 3)** | GPIO 16 | GPIO 2 |

## Building and Running
### Prerequisites
- ESP-IDF 5.x environment sourced: `. $IDF_PATH/export.sh`

### Commands
- **Controller:**
  ```bash
  cd controller
  idf.py build
  idf.py -p /dev/ttyUSB0 flash monitor
  ```
- **Worker:**
  ```bash
  cd worker
  idf.py build
  idf.py -p /dev/ttyACM0 flash  # or /dev/ttyACM1
  ```

## Development Conventions
- **Memory Allocation:** SPI buffers **must** use `heap_caps_calloc(..., MALLOC_CAP_DMA)` for hardware compatibility.
- **Structure Alignment:** All protocol structures must use `__attribute__((packed))` to ensure identical layout between the different ESP32 architectures.
- **Synchronization:** Always use `mac_mutex` when accessing or modifying the `mac_table` to prevent crashes between the sniffer callback and the SPI loop.
- **Reliability:** If communication breaks, prefer lowering the clock speed (e.g., to 500kHz) and increasing `.cs_ena_pretrans` before altering the synchronization logic.
