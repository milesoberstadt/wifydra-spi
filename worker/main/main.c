#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"

static const char *TAG = "WORKER";

// Seeed Studio ESP32-S3 GPIOs
#define GPIO_SCLK 7
#define GPIO_MISO 8
#define GPIO_MOSI 9
#define GPIO_CS   2

/**
 * SPI SLAVE TRANSACTION SIZE LIMITS:
 * 1. DMA Alignment: Buffers must be word-aligned and allocated in DMA-capable RAM.
 * 2. Maximum Size: The default limit is 4092 bytes. To exceed this, 'max_transfer_sz' 
 *    must be set in 'spi_bus_config_t'.
 * 3. Stability: Large transactions (e.g., >32KB) increase the risk of desync if the 
 *    master's clock is too fast or if the slave CPU is heavily loaded.
 */
#define PAYLOAD_SIZE 5100
#define HANDSHAKE_CMD 0xAB
#define HANDSHAKE_ACK 0xBA

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

void app_main(void) {
    ESP_LOGI(TAG, "Starting up... Waiting 1 second.");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 1. SPI Slave Configuration
    spi_bus_config_t buscfg = {
        .miso_io_num = GPIO_MISO,
        .mosi_io_num = GPIO_MOSI,
        .sclk_io_num = GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PAYLOAD_SIZE + 100
    };

    spi_slave_interface_config_t slvcfg = {
        .mode = 0,
        .spics_io_num = GPIO_CS,
        .queue_size = 10,
        .flags = 0,
    };

    // Enable DMA
    ESP_ERROR_CHECK(spi_slave_initialize(SPI2_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO));

    // Allocate structured buffers
    worker_msg_t *tx_frame = heap_caps_calloc(1, sizeof(worker_msg_t), MALLOC_CAP_DMA);
    controller_msg_t *rx_frame = heap_caps_calloc(1, sizeof(controller_msg_t), MALLOC_CAP_DMA);
    
    if (tx_frame == NULL || rx_frame == NULL) {
        ESP_LOGE(TAG, "Failed to allocate SPI structured buffers!");
        return;
    }

    // Prepare fixed payload response
    tx_frame->status = HANDSHAKE_ACK;
    memset(tx_frame->data, 'A', sizeof(tx_frame->data)); 
    tx_frame->checksum = calculate_checksum(tx_frame->data, sizeof(tx_frame->data));

    ESP_LOGI(TAG, "Worker ready. Entering main loop (Structured Mode).");

    // Define transactions for continuous queuing
    spi_slave_transaction_t t1 = {
        .length = PAYLOAD_SIZE * 8,
        .tx_buffer = tx_frame,
        .rx_buffer = rx_frame,
    };
    spi_slave_transaction_t t2 = t1; // Use two slots to keep hardware busy

    ESP_ERROR_CHECK(spi_slave_queue_trans(SPI2_HOST, &t1, portMAX_DELAY));
    ESP_ERROR_CHECK(spi_slave_queue_trans(SPI2_HOST, &t2, portMAX_DELAY));

    while (1) {
        spi_slave_transaction_t *ret_trans;
        
        // Wait for next finished transaction
        ESP_ERROR_CHECK(spi_slave_get_trans_result(SPI2_HOST, &ret_trans, portMAX_DELAY));
        
        if (rx_frame->command == HANDSHAKE_CMD) {
            ESP_LOGI(TAG, "Recv: CH=%d, GPS=%.4f,%.4f", 
                     rx_frame->wifi_channel, 
                     rx_frame->gps.latitude, 
                     rx_frame->gps.longitude);
        }

        // Re-queue the buffer that just finished
        ESP_ERROR_CHECK(spi_slave_queue_trans(SPI2_HOST, ret_trans, portMAX_DELAY));
    }
}
