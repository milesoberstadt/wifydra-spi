#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "WORKER";

#define GPIO_SCLK 7
#define GPIO_MISO 8
#define GPIO_MOSI 9
#define GPIO_CS   2

#define PAYLOAD_SIZE 5100
#define HANDSHAKE_CMD 0xAB
#define HANDSHAKE_ACK 0xBA

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
} wigle_record_t;

#define MAX_MACS 50
static wigle_record_t *mac_table = NULL;
static int mac_count = 0;
static uint8_t current_wifi_ch = 1;
static uint32_t current_unix_time = 0;
static gps_coords_t current_gps = {0};
static SemaphoreHandle_t mac_mutex = NULL;

uint16_t calculate_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

void parse_beacon_ie(const uint8_t *payload, size_t len, char *ssid, char *cap) {
    size_t offset = 36;
    strcpy(ssid, "<hidden>");
    strcpy(cap, "[ESS]");
    while (offset + 2 <= len) {
        uint8_t tag_id = payload[offset];
        uint8_t tag_len = payload[offset + 1];
        if (offset + 2 + tag_len > len) break;
        if (tag_id == 0 && tag_len > 0) {
            int slen = tag_len > 32 ? 32 : tag_len;
            memcpy(ssid, &payload[offset + 2], slen);
            ssid[slen] = '\0';
        } else if (tag_id == 48) strcpy(cap, "[WPA2-PSK-CCMP][ESS]");
        offset += 2 + tag_len;
    }
}

void wifi_sniffer_packet_handler(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT || mac_table == NULL || mac_mutex == NULL) return;
    
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 36 || pkt->payload[0] != 0x80) return;

    if (xSemaphoreTake(mac_mutex, 0) == pdTRUE) {
        uint8_t *bssid = &pkt->payload[10];
        bool found = false;
        for (int i = 0; i < mac_count; i++) {
            if (memcmp(mac_table[i].bssid, bssid, 6) == 0) {
                mac_table[i].hits++;
                mac_table[i].rssi = pkt->rx_ctrl.rssi;
                found = true;
                break;
            }
        }
        if (!found && mac_count < MAX_MACS) {
            wigle_record_t *r = &mac_table[mac_count];
            memcpy(r->bssid, bssid, 6);
            r->hits = 1;
            r->first_seen = current_unix_time;
            r->channel = current_wifi_ch;
            r->rssi = pkt->rx_ctrl.rssi;
            r->lat = current_gps.latitude;
            r->lon = current_gps.longitude;
            parse_beacon_ie(pkt->payload, pkt->rx_ctrl.sig_len, r->ssid, r->capabilities);
            mac_count++;
            ESP_LOGI(TAG, "New AP discovered: %s [%02X:%02X:%02X:%02X:%02X:%02X]", 
                     r->ssid, r->bssid[0], r->bssid[1], r->bssid[2], 
                     r->bssid[3], r->bssid[4], r->bssid[5]);
        }
        xSemaphoreGive(mac_mutex);
    }
}

void app_main(void) {
    // 1. Fundamental system init (Wait for stability)
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Initializing memory...");

    // 2. Allocate resources FIRST before starting any tasks/interrupts
    mac_mutex = xSemaphoreCreateMutex();
    mac_table = heap_caps_calloc(MAX_MACS, sizeof(wigle_record_t), MALLOC_CAP_INTERNAL);
    
    worker_msg_t *tx = heap_caps_calloc(1, sizeof(worker_msg_t), MALLOC_CAP_DMA);
    controller_msg_t *rx = heap_caps_calloc(1, sizeof(controller_msg_t), MALLOC_CAP_DMA);

    if (!mac_mutex || !mac_table || !tx || !rx) {
        ESP_LOGE(TAG, "Fatal: Memory allocation failed");
        esp_restart();
    }

    // 3. Initialize NVS
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    // 4. Start WiFi
    ESP_LOGI(TAG, "Starting WiFi...");
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);

    // 5. Start SPI
    ESP_LOGI(TAG, "Starting SPI...");
    spi_bus_config_t buscfg = {
        .miso_io_num = GPIO_MISO, .mosi_io_num = GPIO_MOSI, .sclk_io_num = GPIO_SCLK,
        .max_transfer_sz = PAYLOAD_SIZE + 100
    };
    spi_slave_interface_config_t slvcfg = {
        .mode = 0, .spics_io_num = GPIO_CS, .queue_size = 3
    };
    spi_slave_initialize(SPI2_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);

    ESP_LOGI(TAG, "Worker fully operational.");

    while (1) {
        tx->status = HANDSHAKE_ACK;
        if (xSemaphoreTake(mac_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            tx->reserved = (uint8_t)(mac_count & 0xFF);
            size_t bytes = mac_count * sizeof(wigle_record_t);
            if (bytes > sizeof(tx->data)) bytes = sizeof(tx->data);
            memcpy(tx->data, mac_table, bytes);
            xSemaphoreGive(mac_mutex);
        }
        tx->checksum = calculate_checksum(tx->data, sizeof(tx->data));

        spi_slave_transaction_t t = {
            .length = PAYLOAD_SIZE * 8, .tx_buffer = tx, .rx_buffer = rx
        };
        
        if (spi_slave_transmit(SPI2_HOST, &t, portMAX_DELAY) == ESP_OK) {
            if (rx->command == HANDSHAKE_CMD) {
                current_unix_time = rx->unix_time;
                current_gps = rx->gps;
                if (rx->wifi_channel >= 1 && rx->wifi_channel <= 14 && rx->wifi_channel != current_wifi_ch) {
                    current_wifi_ch = rx->wifi_channel;
                    ESP_LOGI(TAG, "Switching to WiFi channel %d", current_wifi_ch);
                    esp_wifi_set_channel(current_wifi_ch, WIFI_SECOND_CHAN_NONE);
                }
            }
        }
    }
}
