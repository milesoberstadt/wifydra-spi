# SPI Communication Project Plan: ESP32 Controller to 3x ESP32-S3 Workers

### Scaling to 15 Workers (6GHz PSC channels) [TODO]
- **CS Multiplexing:** 
    - Implement a **4-to-16 Decoder** (e.g., 74HC154) or **GPIO Expander** (e.g., MCP23017) to control 15 CS lines using only 4 controller pins.
    - Software: Update SPI polling loop to set the address on the decoder pins before each transaction.
- **Bus Integrity:** 
    - Evaluate need for SPI bus buffers/drivers (e.g., 74HC244) to handle increased capacitance of 15 slaves.
    - Implement per-worker channel locking (Worker 1 -> Ch 1, Worker 2 -> Ch 2, etc.).
- **Power Management:** Investigate current draw for 15 workers + GPS + SD card

---

## Upcoming Tasks
0. [ ] Update to ESP-IDF 6
1. [ ] Prototype Phase 9 CS multiplexing hardware.
2. [ ] Evaluate SPI bus buffers for 11-worker scale.

