#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "CONTROLLER";

#define GPIO_SCLK 18
#define GPIO_MISO 19
#define GPIO_MOSI 23
#define GPIO_CS1   5
#define GPIO_CS2   4
#define GPIO_CS3  16

/**
 * SPI TRANSACTION SIZE LIMITS:
 * 1. Default Limit: Without setting 'max_transfer_sz', the SPI driver defaults to 4092 bytes
 *    when DMA is enabled.
 * 2. Hardware/Driver Max: By setting 'max_transfer_sz', you can technically transfer up to
 *    64KB or more, provided you have a contiguous block of DMA-capable RAM.
 * 3. Safe Practice: For payloads over 4KB, always explicitly set 'max_transfer_sz' in the 
 *    bus configuration and use 'MALLOC_CAP_DMA' for buffer allocation.
 */
#define PAYLOAD_SIZE 5100
 // Extra buffer for checksum/header
#define HANDSHAKE_CMD 0xAB
#define HANDSHAKE_ACK 0xBA

spi_device_handle_t workers[3];

// Simple additive checksum for verification
uint16_t calculate_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint16_t)(sum & 0xFFFF);
}

typedef struct __attribute__((packed)) {
    float latitude;
    float longitude;
} gps_coords_t;

typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t wifi_channel;
    gps_coords_t gps;
    uint8_t padding[PAYLOAD_SIZE - sizeof(uint8_t) - sizeof(uint8_t) - sizeof(gps_coords_t)];
} controller_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t reserved;
    uint16_t checksum;
    uint8_t data[PAYLOAD_SIZE - 4];
} worker_msg_t;

// Wifi Configuration Logic
static const uint8_t popular_channels[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10, 14};
// Map: CS Index -> Current Channel
static uint8_t worker_current_channels[3]; 

static const gps_coords_t static_gps = {
    .latitude = 37.7749f,  // San Francisco
    .longitude = -122.4194f
};

void app_main(void) {
    ESP_LOGI(TAG, "Starting up... Waiting 1 second for workers.");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Initialize channels from popularity list
    for(int i=0; i<3; i++) worker_current_channels[i] = popular_channels[i];

    // 1. SPI Bus Configuration
    spi_bus_config_t buscfg = {
        .miso_io_num = GPIO_MISO,
        .mosi_io_num = GPIO_MOSI,
        .sclk_io_num = GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PAYLOAD_SIZE + 100
    };

    // Initialize the SPI bus
    ESP_ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 2. Worker Device Configurations
    int cs_pins[3] = {GPIO_CS1, GPIO_CS2, GPIO_CS3};
    for (int i = 0; i < 3; i++) {
        spi_device_interface_config_t devcfg = {
            .clock_speed_hz = 1 * 1000 * 1000, // 1 MHz
            .mode = 0,                         // SPI Mode 0
            .spics_io_num = cs_pins[i],        // CS pin for this worker
            .queue_size = 7,
            .cs_ena_pretrans = 2,              // More setup time
        };
        ESP_ERROR_CHECK(spi_bus_add_device(HSPI_HOST, &devcfg, &workers[i]));
    }

    // Allocate structured buffers
    controller_msg_t *tx_frame = heap_caps_calloc(1, sizeof(controller_msg_t), MALLOC_CAP_DMA);
    worker_msg_t *rx_frame = heap_caps_calloc(1, sizeof(worker_msg_t), MALLOC_CAP_DMA);

    if (tx_frame == NULL || rx_frame == NULL) {
        ESP_LOGE(TAG, "Failed to allocate SPI structured buffers!");
        return;
    }
    
    while (1) {
        for (int i = 0; i < 3; i++) {
            ESP_LOGI(TAG, "--- Checking Worker %d (CS Pin %d) ---", i + 1, cs_pins[i]);

            // Prepare Frame
            memset(tx_frame, 0, sizeof(controller_msg_t));
            tx_frame->command = HANDSHAKE_CMD;
            tx_frame->wifi_channel = worker_current_channels[i];
            tx_frame->gps = static_gps;

            spi_transaction_t t = {
                .length = PAYLOAD_SIZE * 8,
                .tx_buffer = tx_frame,
                .rx_buffer = rx_frame,
            };

            esp_err_t ret = spi_device_transmit(workers[i], &t);
            if (ret == ESP_OK) {
                if (rx_frame->status == HANDSHAKE_ACK) {
                    uint16_t calculated = calculate_checksum(rx_frame->data, sizeof(rx_frame->data));

                    if (rx_frame->checksum == calculated) {
                        ESP_LOGI(TAG, "Success! Worker %d (CH %d) Checksum OK.", i + 1, tx_frame->wifi_channel);
                    } else {
                        ESP_LOGE(TAG, "Checksum mismatch! Worker %d Recv: 0x%04X, Calc: 0x%04X", i + 1, rx_frame->checksum, calculated);
                    }
                } else {
                    ESP_LOGW(TAG, "Worker %d failed handshake (Rx Status: 0x%02X)", i + 1, rx_frame->status);
                }
            } else {
                ESP_LOGE(TAG, "Transmission failed for Worker %d: %s", i + 1, esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "Cycle complete. Waiting 10s before next pass.");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
