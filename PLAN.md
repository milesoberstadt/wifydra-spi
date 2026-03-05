# SPI Communication Project Plan: ESP32 Controller to 3x ESP32-S3 Workers

This document outlines the architecture, hardware connections, and implementation strategy for establishing reliable SPI communication between one ESP32 (Controller) and three ESP32-S3 (Workers) using the ESP-IDF framework.

## 1. Project Overview
- **Controller:** ESP32-WROOM-32 (NodeMCU-32S style)
- **Workers:** 3x Seeed Studio ESP32-S3 (Xiao or similar)
- **Protocol:** SPI (Serial Peripheral Interface)
- **Goal:** Controller cycles through workers, assigns WiFi channels, and receives WiFi beacon tracking data (Wigle format).

---

## 2. Hardware Wiring Diagram

All devices share a common **GND**.

### SPI Bus Connections (Common)
| Signal | ESP32 (Controller) | ESP32-S3 (Worker 1) | ESP32-S3 (Worker 2) | ESP32-S3 (Worker 3) |
| :--- | :--- | :--- | :--- | :--- |
| **SCLK** | GPIO 18 | GPIO 7 | GPIO 7 | GPIO 7 |
| **MISO** | GPIO 19 | GPIO 8 | GPIO 8 | GPIO 8 |
| **MOSI** | GPIO 23 | GPIO 9 | GPIO 9 | GPIO 9 |
| **GND** | GND | GND | GND | GND |

### Chip Select (CS) Connections (Unique)
| Target | ESP32 (Controller CS Pin) | ESP32-S3 (Worker CS Pin) |
| :--- | :--- | :--- |
| **Worker 1** | GPIO 5 | GPIO 2 |
| **Worker 2** | GPIO 4 | GPIO 2 |
| **Worker 3** | GPIO 16 | GPIO 2 |

---

## 3. SPI Configuration Strategy

- **SPI Mode:** Mode 0 (CPOL=0, CPHA=0).
- **Clock Speed:** 1 MHz.
- **DMA:** Enabled on both sides for 5100-byte payloads.
- **Transaction:** Fixed-frame single transaction model for atomic command/response.

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
