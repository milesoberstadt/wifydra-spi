# ESP32 Distributed WiFi Sniffer (SPI Controller-Worker)

A distributed WiFi beacon sniffing system using an ESP32 as a central Controller and multiple ESP32-S3 nodes as Workers. The system uses a high-speed, robust SPI protocol to synchronize time, GPS coordinates, and aggregate WiFi discovery data.

## Features
- **Centralized Control:** One Controller (SPI Master) manages channel assignments and time/GPS sync for all workers.
- **High-Speed SPI:** Custom fixed-frame protocol (5100 bytes) running at 1MHz with continuous hardware queuing.
- **Advanced Sniffing:** Workers operate in promiscuous mode, parsing 802.11 beacon frames to extract SSIDs, BSSIDs, and security capabilities.
- **Wigle Compatibility:** Data is stored in Wigle-compatible structures with first-seen timestamps and RSSI tracking.
- **Robustness:** 
    - **Sent Tracking:** Workers only transmit new discoveries to save bandwidth.
    - **Smart Overwrite:** Automatically recycles memory by overwriting previously sent records when full (500 AP limit).
    - **Memory Safety:** Mutex-protected state and DMA-aligned buffers.

## Hardware Wiring
Refer to [PLAN.md](PLAN.md) for the detailed GPIO pinout and wiring diagram.

## Project Structure
- `/controller`: ESP-IDF source for the SPI Master node.
- `/worker`: ESP-IDF source for the SPI Slave nodes (WiFi Sniffers).
- `GEMINI.md`: Instructional context and engineering standards for the project.
- `PLAN.md`: Implementation history and completed milestones.

## Getting Started
See [GEMINI.md](GEMINI.md) for build, flash, and monitoring commands.
