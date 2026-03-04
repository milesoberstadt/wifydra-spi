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
    uint8_t reserved; // Number of records in this packet
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
    uint8_t sent; // 0 = unsent, 1 = sent
} wigle_record_t;

#define MAX_MACS 500
#define RECORDS_PER_BUFFER 50 // How many fit in 5096 bytes (98 bytes each)

static wigle_record_t *mac_table = NULL;
static int mac_count = 0;
static uint8_t current_wifi_ch = 0;
static uint32_t current_unix_time = 0;
static gps_coords_t current_gps = {0};
static SemaphoreHandle_t mac_mutex = NULL;

// Indices of records currently being sent
static int in_flight_indices[RECORDS_PER_BUFFER];
static int in_flight_count = 0;

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
    if (type != WIFI_PKT_MGMT || current_wifi_ch == 0 || mac_table == NULL || mac_mutex == NULL) return;
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
            r->sent = 0;
            parse_beacon_ie(pkt->payload, pkt->rx_ctrl.sig_len, r->ssid, r->capabilities);
            mac_count++;
            ESP_LOGI(TAG, "Ch %d | New AP: %s", current_wifi_ch, r->ssid);
        }
        xSemaphoreGive(mac_mutex);
    }
}

void wifi_init_sniffer(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler));
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Worker ready.");
    
    mac_mutex = xSemaphoreCreateMutex();
    mac_table = heap_caps_calloc(MAX_MACS, sizeof(wigle_record_t), MALLOC_CAP_INTERNAL);

    wifi_init_sniffer();

    spi_bus_config_t buscfg = {
        .miso_io_num = GPIO_MISO, .mosi_io_num = GPIO_MOSI, .sclk_io_num = GPIO_SCLK,
        .max_transfer_sz = PAYLOAD_SIZE + 100
    };
    spi_slave_interface_config_t slvcfg = {
        .mode = 0, .spics_io_num = GPIO_CS, .queue_size = 3
    };
    spi_slave_initialize(SPI2_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);

    worker_msg_t *tx = heap_caps_calloc(1, sizeof(worker_msg_t), MALLOC_CAP_DMA);
    controller_msg_t *rx = heap_caps_calloc(1, sizeof(controller_msg_t), MALLOC_CAP_DMA);

    while (1) {
        // 1. Prepare buffer with oldest unsent records
        tx->status = HANDSHAKE_ACK;
        memset(tx->data, 0, sizeof(tx->data));
        in_flight_count = 0;

        if (xSemaphoreTake(mac_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < mac_count && in_flight_count < RECORDS_PER_BUFFER; i++) {
                if (mac_table[i].sent == 0) {
                    memcpy(tx->data + (in_flight_count * sizeof(wigle_record_t)), &mac_table[i], sizeof(wigle_record_t));
                    in_flight_indices[in_flight_count] = i;
                    in_flight_count++;
                }
            }
            tx->reserved = (uint8_t)in_flight_count;
            xSemaphoreGive(mac_mutex);
        }
        tx->checksum = calculate_checksum(tx->data, sizeof(tx->data));

        // 2. Transmit
        spi_slave_transaction_t t = {
            .length = PAYLOAD_SIZE * 8, .tx_buffer = tx, .rx_buffer = rx
        };
        
        if (spi_slave_transmit(SPI2_HOST, &t, portMAX_DELAY) == ESP_OK) {
            // 3. Mark as sent on success
            if (in_flight_count > 0) {
                xSemaphoreTake(mac_mutex, portMAX_DELAY);
                for (int i = 0; i < in_flight_count; i++) {
                    mac_table[in_flight_indices[i]].sent = 1;
                }
                xSemaphoreGive(mac_mutex);
            }

            // 4. Process incoming controller data
            if (rx->wifi_channel >= 1 && rx->wifi_channel <= 14) {
                current_unix_time = rx->unix_time;
                current_gps = rx->gps;

                if (rx->wifi_channel != current_wifi_ch) {
                    ESP_LOGI(TAG, "New Channel: %d. Resetting table.", rx->wifi_channel);
                    xSemaphoreTake(mac_mutex, portMAX_DELAY);
                    mac_count = 0;
                    memset(mac_table, 0, MAX_MACS * sizeof(wigle_record_t));
                    current_wifi_ch = rx->wifi_channel;
                    esp_wifi_set_promiscuous(false);
                    esp_wifi_set_channel(current_wifi_ch, WIFI_SECOND_CHAN_NONE);
                    esp_wifi_set_promiscuous(true);
                    xSemaphoreGive(mac_mutex);
                }
            }
        }
    }
}
