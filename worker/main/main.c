#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "WORKER";

// Seeed Studio ESP32-S3 GPIOs
#define GPIO_SCLK 7
#define GPIO_MISO 8
#define GPIO_MOSI 9
#define GPIO_CS   2

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
        .max_transfer_sz = PAYLOAD_SIZE + 100 // Required for DMA transfers > 4092 bytes
    };

    spi_slave_interface_config_t slvcfg = {
        .mode = 0,
        .spics_io_num = GPIO_CS,
        .queue_size = 10,
        .flags = 0,
    };

    // Enable DMA
    ESP_ERROR_CHECK(spi_slave_initialize(SPI2_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO));

    // Use calloc to ensure zero-initialized DMA-safe memory
    uint8_t *sendbuf = heap_caps_calloc(1, PAYLOAD_SIZE, MALLOC_CAP_DMA);
    uint8_t *recvbuf = heap_caps_calloc(1, PAYLOAD_SIZE, MALLOC_CAP_DMA);
    uint8_t *handshake_ack_buf = heap_caps_calloc(1, 4, MALLOC_CAP_DMA);
    uint8_t *handshake_rx_buf = heap_caps_calloc(1, 4, MALLOC_CAP_DMA);
    
    if (sendbuf == NULL || recvbuf == NULL || handshake_ack_buf == NULL || handshake_rx_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate SPI buffers!");
        return;
    }

    // Prepare large dummy payload for SINGLE transaction model
    // [ACK (1 byte)] [Padding (1 byte)] [Checksum (2 bytes)] [Data (5096 bytes)]
    memset(sendbuf, 0, PAYLOAD_SIZE);
    sendbuf[0] = HANDSHAKE_ACK;
    memset(sendbuf + 4, 'A', PAYLOAD_SIZE - 4); 
    uint16_t checksum = calculate_checksum(sendbuf + 4, PAYLOAD_SIZE - 4);
    memcpy(sendbuf + 2, &checksum, sizeof(uint16_t));

    ESP_LOGI(TAG, "Worker ready. Entering main loop (Continuous Queuing).");

    while (1) {
        spi_slave_transaction_t t = {
            .length = PAYLOAD_SIZE * 8,
            .tx_buffer = sendbuf,
            .rx_buffer = recvbuf,
        };

        // Wait indefinitely for the master to initiate a transaction
        esp_err_t ret = spi_slave_transmit(SPI2_HOST, &t, portMAX_DELAY);
        
        if (ret == ESP_OK) {
            if (recvbuf[0] == HANDSHAKE_CMD) {
                ESP_LOGI(TAG, "Success! Handled full transaction.");
            } else {
                ESP_LOGW(TAG, "Received unexpected command: 0x%02X", recvbuf[0]);
            }
        } else {
            ESP_LOGE(TAG, "SPI Slave transmission failed: %s", esp_err_to_name(ret));
        }
    }
}
