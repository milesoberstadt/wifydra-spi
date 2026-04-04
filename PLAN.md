# SPI Communication Project Plan: ESP32 Controller to 3x ESP32-S3 Workers

This document outlines the architecture, hardware connections, and implementation strategy for establishing reliable SPI communication between one ESP32 (Controller) and three ESP32-S3 (Workers) using the ESP-IDF framework.

## 1. Project Overview
- **Controller:** ESP32-WROOM-32 (NodeMCU-32S style)
- **Workers:** 3x Seeed Studio ESP32-S3 (Initially), scaling to 11.
- **Protocol:** SPI (Serial Peripheral Interface)
- **Goal:** Controller cycles through workers, assigns WiFi channels, and receives WiFi beacon tracking data (Wigle format).

---

## 2. Hardware Wiring Diagram

All devices share a common **GND**.

### SPI Bus Connections (Common)
| Signal | ESP32 (Controller) | ESP32-S3 (Worker x) | SD Card (SPI Mode) |
| :--- | :--- | :--- | :--- |
| **SCLK** | GPIO 18 | GPIO 7 | SCLK |
| **MISO** | GPIO 19 | GPIO 8 | MISO |
| **MOSI** | GPIO 23 | GPIO 9 | MOSI |
| **GND** | GND | GND | GND |

### Chip Select (CS) Connections (Unique)
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

---

## 3. SPI Configuration Strategy

- **SPI Mode:** Mode 0 (CPOL=0, CPHA=0).
- **Clock Speed:** 1 MHz (Shared between workers and SD card).
- **DMA:** Enabled on both sides for 5100-byte payloads.
- **Transaction:** Fixed-frame single transaction model for atomic command/response.
- **Robustness:** 
    - Initialized all CS pins HIGH early to prevent bus contention.
    - SD card must be the first device initialized on the bus to switch to SPI mode.
    - Internal pull-ups enabled on MISO, MOSI, and SCLK.

---

## 4. Implementation Phases

### Phase 1: Project Scaffolding [DONE]
- Create `controller/` and `worker/` directories.
- Initialize ESP-IDF projects.

### Phase 2: Basic Handshake [DONE]
- Verified CS toggling and basic byte exchange.

### Phase 3: Large Payload & Checksum [DONE]
- Implemented 5100-byte DMA transfers.
- Added 16-bit additive checksum validation.

### Phase 4: Controller Validation & Cycling [DONE]
- Controller successfully polls all three workers in sequence.

### Phase 5: Troubleshooting & Signal Integrity [DONE]
- Implemented "Continuous Queuing" on Workers.
- Increased `.cs_ena_pretrans` for DMA stability.
- Stabilized 1MHz communication.

### Phase 6: WiFi Sniffer Integration [DONE]
- Implemented Promiscuous mode on Workers.
- Added Beacon frame filtering and IE parsing (SSID/Capabilities).
- Implemented Wigle-compatible record tracking with "Sent" status and Smart Overwrite.

### Phase 7: GPS Integration (NEO-6M) [DONE]
- **UART Setup:** Configure UART2 for NMEA data at 9600 baud.
- **NMEA Parsing:** Implement parser to extract Lat, Lon, Altitude, and Time.
- **Warm Start Optimization:** 
    - Store last-known coordinates in NVS (flash).
    - **UBX Helper:** Send `UBX-AID-INI` on boot to seed location for faster lock.
- **Sync:** Pass live GPS and Unix time to workers via SPI.

### Phase 8: Persistent Storage (SDSPI) [DONE]
- **Mounting:** Initialize SDSPI on shared bus (HSPI_HOST) with GPIO 21 CS.
- **LFN Support:** Enabled Long File Name support in FATFS.
- **Logging:** 
    - Descriptive naming: `YYYYMMDD_HHMMSS.csv`.
    - **Buffered IO:** 4KB RAM buffer to batch writes and reduce SD wear.
    - Periodic flush after each worker polling cycle.

### Phase 9: Scaling to 11 Workers (Full 2.4GHz Spectrum) [TODO]
- **CS Multiplexing:** 
    - Implement a **4-to-16 Decoder** (e.g., 74HC154) or **GPIO Expander** (e.g., MCP23017) to control 11 CS lines using only 4 controller pins.
    - Software: Update SPI polling loop to set the address on the decoder pins before each transaction.
- **Bus Integrity:** 
    - Evaluate need for SPI bus buffers/drivers (e.g., 74HC244) to handle increased capacitance of 11 slaves.
    - Implement per-worker channel locking (Worker 1 -> Ch 1, Worker 2 -> Ch 2, etc.).
- **Power Management:** Monitor total current draw; each ESP32-S3 can peak at ~300mA during WiFi activity.

---

## 5. Completed Tasks

1. [x] Setup Project Structure (ESP-IDF boilerplate).
2. [x] Implement SPI Master Driver on Controller.
3. [x] Implement SPI Slave Driver on Worker.
4. [x] Implement Worker cycling logic on Controller.
5. [x] Implement Large Message Generation & Checksum on Worker.
6. [x] Implement Reception & Verification on Controller.
7. [x] Implement WiFi Beacon Sniffing and MAC tracking.
8. [x] Implement Wigle-compatible data structures and SPI reporting.
9. [x] Optimize for 1MHz stability with double buffering.
10. [x] Implement UART2 driver for NEO-6M on Controller.
11. [x] Implement NMEA parsing for RMC sentences.
12. [x] Implement NVS storage for last-known GPS location.
13. [x] Implement UBX protocol helper for assisted cold-start.
14. [x] Transition from SDMMC to SDSPI for shared bus compatibility.
15. [x] Implement 4KB buffered logging to extend SD card life.
16. [x] Enable Long File Name support and YYYYMMDD filename formatting.

## 6. Upcoming Tasks
1. [ ] Prototype Phase 9 CS multiplexing hardware.
2. [ ] Evaluate SPI bus buffers for 11-worker scale.

## 7. Shopping list
1. SDMMC compatible microsd sniffer
2. USB bulkhead
3. CS Multiplexer (74HC154 or MCP23017)
4. 15x XIAO ESP32-C6 modules
5. 15x 10k resistors
6. 18650 Battery Holder - https://a.co/d/02SaYOTm
