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

#define PAYLOAD_SIZE 5100
#define HANDSHAKE_CMD 0xAB
#define HANDSHAKE_ACK 0xBA

spi_device_handle_t workers[3];

uint16_t calculate_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

typedef struct __attribute__((packed)) {
    float latitude;
    float longitude;
} gps_coords_t;

typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t wifi_channel;
    uint32_t unix_time;
    gps_coords_t gps;
    uint8_t padding[5100 - 14];
} controller_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t reserved;
    uint16_t checksum;
    uint8_t data[5100 - 4];
} worker_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t bssid[6];
    char ssid[33];
    char capabilities[40];
    uint32_t first_seen;
    uint8_t channel;
    int8_t rssi;
    float lat;
    float lon;
    uint32_t hits;
    uint8_t sent;
} wigle_record_t;

static const uint8_t popular_channels[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10, 14};
static uint8_t worker_current_channels[3]; 
static const gps_coords_t static_gps = { .latitude = 37.7749f, .longitude = -122.4194f };

void app_main(void) {
    ESP_LOGI(TAG, "Controller starting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    for(int i=0; i<3; i++) worker_current_channels[i] = popular_channels[i];

    spi_bus_config_t buscfg = {
        .miso_io_num = GPIO_MISO, .mosi_io_num = GPIO_MOSI, .sclk_io_num = GPIO_SCLK,
        .max_transfer_sz = PAYLOAD_SIZE + 100
    };
    ESP_ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    int cs_pins[3] = {GPIO_CS1, GPIO_CS2, GPIO_CS3};
    for (int i = 0; i < 3; i++) {
        spi_device_interface_config_t devcfg = {
            .clock_speed_hz = 1 * 1000 * 1000, 
            .mode = 0,
            .spics_io_num = cs_pins[i],
            .queue_size = 7,
            .cs_ena_pretrans = 2,
        };
        ESP_ERROR_CHECK(spi_bus_add_device(HSPI_HOST, &devcfg, &workers[i]));
    }

    controller_msg_t *tx = heap_caps_calloc(1, sizeof(controller_msg_t), MALLOC_CAP_DMA);
    worker_msg_t *rx = heap_caps_calloc(1, sizeof(worker_msg_t), MALLOC_CAP_DMA);
    uint32_t simulated_time = 1740950400; 

    while (1) {
        for (int i = 0; i < 3; i++) {
            ESP_LOGI(TAG, "Polling Worker %d...", i + 1);
            memset(tx, 0, sizeof(controller_msg_t));
            tx->command = HANDSHAKE_CMD;
            tx->wifi_channel = worker_current_channels[i];
            tx->unix_time = simulated_time;
            tx->gps = static_gps;

            spi_transaction_t t = { .length = PAYLOAD_SIZE * 8, .tx_buffer = tx, .rx_buffer = rx };
            if (spi_device_transmit(workers[i], &t) == ESP_OK) {
                if (rx->status == HANDSHAKE_ACK) {
                    uint16_t cal = calculate_checksum(rx->data, sizeof(rx->data));
                    if (rx->checksum == cal) {
                        int count = rx->reserved;
                        ESP_LOGI(TAG, "Worker %d OK. Records in packet: %d", i+1, count);
                        wigle_record_t *rec = (wigle_record_t *)rx->data;
                        for (int j = 0; j < (count > 2 ? 2 : count); j++) {
                            ESP_LOGI(TAG, "  AP: %s (%02X:%02X:%02X:%02X:%02X:%02X) RSSI:%d Hits:%lu", 
                                     rec[j].ssid, rec[j].bssid[0], rec[j].bssid[1], rec[j].bssid[2],
                                     rec[j].bssid[3], rec[j].bssid[4], rec[j].bssid[5], rec[j].rssi, (unsigned long)rec[j].hits);
                        }
                    } else ESP_LOGE(TAG, "Worker %d Checksum Fail!", i+1);
                } else ESP_LOGW(TAG, "Worker %d Handshake Fail (0x%02X)", i+1, rx->status);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        simulated_time += 15;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
