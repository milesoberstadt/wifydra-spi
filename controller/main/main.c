#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include <errno.h>

static const char *TAG = "CONTROLLER";

// --- Configuration ---
#define CONFIG_WAIT_FOR_GPS_FIX true  // If true, controller won't poll workers until GPS has a fix
// ---------------------

#define GPIO_SCLK 18
#define GPIO_MISO 19
#define GPIO_MOSI 23
#define GPIO_CS1   5
#define GPIO_CS2   4
#define GPIO_CS3  32
#define GPIO_LED  27

// GPS UART Config
#define GPS_UART_NUM UART_NUM_2
#define GPS_TX_PIN 17
#define GPS_RX_PIN 16
#define GPS_BUF_SIZE 1024

#define PAYLOAD_SIZE 5100
#define HANDSHAKE_CMD 0xAB
#define HANDSHAKE_ACK 0xBA

// NVS Keys
#define NVS_NAMESPACE "gps_data"
#define NVS_KEY_LAT "last_lat"
#define NVS_KEY_LON "last_lon"

spi_device_handle_t workers[3];

typedef struct __attribute__((packed)) {
    float latitude;
    float longitude;
} gps_coords_t;

static gps_coords_t current_gps = { .latitude = 0.0f, .longitude = 0.0f };
static uint32_t current_unix_time = 1740950400; // Default start time

uint16_t calculate_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

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

// NMEA Parsing State
static bool gps_data_seen = false;
static bool gps_fix_acquired = false;
static bool sd_card_ready = false;

// SD Card Handle
sdmmc_card_t *card;
#define MOUNT_POINT "/sdcard"

void init_sd_card() {
    ESP_LOGI(TAG, "Initializing SD card (SPI)");

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ESP_LOGI(TAG, "Using SPI peripheral");

    // Host configuration
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = HSPI_HOST; // Use the same host as workers
    host.max_freq_khz = 1000; // Lower to 1MHz for stability

    // Device configuration
    sdspi_device_config_t dev_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_config.gpio_cs = 13; // User specified CS pin
    dev_config.host_id = HSPI_HOST; // Explicitly set host ID
    
    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). Check wiring and pull-ups.", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");
    sd_card_ready = true;
}

// Simple coordinate parser (ddmm.mmmm -> decimal degrees)
float parse_coord(char *str, char dir) {
    if (!str || strlen(str) < 4) return 0.0f;
    float raw = atof(str);
    int deg = (int)(raw / 100);
    float min = raw - (deg * 100);
    float val = deg + (min / 60.0f);
    if (dir == 'S' || dir == 'W') val = -val;
    return val;
}

void save_gps_to_nvs(float lat, float lon) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return;

    nvs_set_u32(my_handle, NVS_KEY_LAT, *((uint32_t*)&lat));
    nvs_set_u32(my_handle, NVS_KEY_LON, *((uint32_t*)&lon));
    nvs_commit(my_handle);
    nvs_close(my_handle);
    ESP_LOGI(TAG, "Saved GPS to NVS: %.4f, %.4f", lat, lon);
}

void load_gps_from_nvs() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved GPS data found.");
        return;
    }

    uint32_t lat_int, lon_int;
    if (nvs_get_u32(my_handle, NVS_KEY_LAT, &lat_int) == ESP_OK &&
        nvs_get_u32(my_handle, NVS_KEY_LON, &lon_int) == ESP_OK) {
        
        current_gps.latitude = *((float*)&lat_int);
        current_gps.longitude = *((float*)&lon_int);
        ESP_LOGI(TAG, "Loaded GPS from NVS: %.4f, %.4f", current_gps.latitude, current_gps.longitude);
    }
    nvs_close(my_handle);
}

// UBX Checksum calculation (Fletcher-8)
void ubx_checksum(uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b) {
    *ck_a = 0;
    *ck_b = 0;
    for (size_t i = 0; i < len; i++) {
        *ck_a += data[i];
        *ck_b += *ck_a;
    }
}

// Send a raw UBX message over UART
void ubx_send_message(uint8_t class, uint8_t id, uint16_t len, uint8_t *payload) {
    uint8_t header[6] = {
        0xB5, 0x62, 
        class, id, 
        (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF)
    };
    uint8_t ck_a, ck_b;
    
    // Checksum is calculated over Class, ID, Length, and Payload
    uint8_t *chk_buf = malloc(len + 4);
    chk_buf[0] = class;
    chk_buf[1] = id;
    chk_buf[2] = header[4];
    chk_buf[3] = header[5];
    if (len > 0) memcpy(&chk_buf[4], payload, len);
    
    ubx_checksum(chk_buf, len + 4, &ck_a, &ck_b);
    free(chk_buf);

    uart_write_bytes(GPS_UART_NUM, header, 6);
    if (len > 0) uart_write_bytes(GPS_UART_NUM, payload, len);
    uint8_t footer[2] = {ck_a, ck_b};
    uart_write_bytes(GPS_UART_NUM, footer, 2);
}

// Assist GPS module with last known position (UBX-AID-INI)
void ubx_assist_cold_start(float lat, float lon) {
    if (lat == 0.0f && lon == 0.0f) return;

    ESP_LOGI(TAG, "Sending UBX assistance: %.4f, %.4f", lat, lon);

    typedef struct __attribute__((packed)) {
        int32_t ecefX_or_lat;
        int32_t ecefY_or_lon;
        int32_t ecefZ_or_alt;
        uint32_t posAcc;
        uint16_t tmCfg;
        uint16_t wn;
        uint32_t tow;
        int32_t towNs;
        int32_t tAcc;
        int32_t fAcc;
        int32_t clkD;
        uint32_t clkDAcc;
        uint32_t flags;
    } ubx_aid_ini_t;

    ubx_aid_ini_t msg = {0};
    msg.ecefX_or_lat = (int32_t)(lat * 1e7);
    msg.ecefY_or_lon = (int32_t)(lon * 1e7);
    msg.posAcc = 100000; // 1km accuracy for assistance
    msg.flags = 0x01 | 0x20; // 0x01 = pos valid, 0x20 = alt invalid (use 2D)

    ubx_send_message(0x0B, 0x01, sizeof(msg), (uint8_t *)&msg);
}

// Logging State
static char current_log_filename[64] = "";
static bool log_file_created = false;

const char* WIGLE_HEADER = "WigleWifi-1.0,appRelease=1.0,model=ESP32-Scanner,release=1.0,device=ESP32-Scanner,display=ESP32-Scanner,board=ESP32,brand=Espressif\n"
                           "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n";

void create_wigle_log(const char* date, const char* time) {
    if (!sd_card_ready || log_file_created || !date || !time) return;

    // Sanitize time: GPS often gives "HHMMSS.SS", we only want "HHMMSS"
    char clean_time[7];
    strncpy(clean_time, time, 6);
    clean_time[6] = '\0';

    // Use a unique 8.3 compliant filename: HHMMSS.csv (6 characters + .csv)
    snprintf(current_log_filename, sizeof(current_log_filename), "%s/%s.csv", MOUNT_POINT, clean_time);
    ESP_LOGI(TAG, "Creating new log file: %s", current_log_filename);

    FILE *f = fopen(current_log_filename, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to create log file! (errno: %d, %s)", errno, strerror(errno));
        return;
    }
    fprintf(f, "%s", WIGLE_HEADER);
    fclose(f);
    log_file_created = true;
}

// $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
void parse_nmea_rmc(char *line) {
    if (!gps_data_seen) {
        ESP_LOGI(TAG, "GPS Data stream detected.");
        gps_data_seen = true;
    }

    if (strncmp(line, "$GPRMC", 6) == 0 || strncmp(line, "$GNRMC", 6) == 0) {
        char *token;
        char *rest = line;
        int field = 0;
        char *time = NULL, *status = NULL, *lat = NULL, *ns = NULL, *lon = NULL, *ew = NULL, *date = NULL;

        while ((token = strsep(&rest, ",")) != NULL) {
            switch (field) {
                case 1: time = token; break;
                case 2: status = token; break;
                case 3: lat = token; break;
                case 4: ns = token; break;
                case 5: lon = token; break;
                case 6: ew = token; break;
                case 9: date = token; break;
            }
            field++;
        }

        // Create log file as soon as we have a date/time (even without fix)
        if (date && time && strlen(date) == 6 && strlen(time) >= 6) {
            create_wigle_log(date, time);
        }

        if (status && strcmp(status, "A") == 0) {
            float new_lat = parse_coord(lat, ns ? ns[0] : 'N');
            float new_lon = parse_coord(lon, ew ? ew[0] : 'E');

            if (!gps_fix_acquired) {
                ESP_LOGI(TAG, "GPS Fix Acquired! Lat: %.4f, Lon: %.4f", new_lat, new_lon);
                gps_fix_acquired = true;
                save_gps_to_nvs(new_lat, new_lon);
            }
            
            current_gps.latitude = new_lat;
            current_gps.longitude = new_lon;
        }
    }
}

void gps_task(void *pvParameters) {
    uint8_t *data = (uint8_t *)malloc(GPS_BUF_SIZE);
    while (1) {
        int len = uart_read_bytes(GPS_UART_NUM, data, GPS_BUF_SIZE - 1, pdMS_TO_TICKS(500));
        if (len > 0) {
            data[len] = '\0';
            parse_nmea_rmc((char *)data);
        }
    }
}

void status_led_task(void *pvParameters) {
    gpio_reset_pin(GPIO_LED);
    gpio_set_direction(GPIO_LED, GPIO_MODE_OUTPUT);
    
    while (1) {
        if (gps_fix_acquired) {
            gpio_set_level(GPIO_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else if (gps_data_seen) {
            gpio_set_level(GPIO_LED, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(GPIO_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            gpio_set_level(GPIO_LED, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Controller starting...");

    // Initialize all CS pins to HIGH early to prevent bus interference
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << GPIO_CS1) | (1ULL << GPIO_CS2) | (1ULL << GPIO_CS3) | (1ULL << 13),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&cs_cfg);
    gpio_set_level(GPIO_CS1, 1);
    gpio_set_level(GPIO_CS2, 1);
    gpio_set_level(GPIO_CS3, 1);
    gpio_set_level(13, 1);
    
    // Start Status LED Task
    xTaskCreate(status_led_task, "status_led_task", 2048, NULL, 5, NULL);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load last known position for warm start
    load_gps_from_nvs();

    // Configure UART for GPS early to send assistance
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, GPS_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Send assistance data to GPS module
    ubx_assist_cold_start(current_gps.latitude, current_gps.longitude);

    spi_bus_config_t buscfg = {
        .miso_io_num = GPIO_MISO, .mosi_io_num = GPIO_MOSI, .sclk_io_num = GPIO_SCLK,
        .max_transfer_sz = PAYLOAD_SIZE + 100
    };
    ESP_ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Enable internal pull-ups for the SPI bus
    gpio_set_pull_mode(GPIO_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY);

    // Initialize SD Card IMMEDIATELY after SPI bus is ready (must be first communication)
    init_sd_card();

    vTaskDelay(pdMS_TO_TICKS(1000));
    for(int i=0; i<3; i++) worker_current_channels[i] = popular_channels[i];

    // Start GPS Task (after SD card is initialized so it can create logs)
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 5, NULL);

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

    if (CONFIG_WAIT_FOR_GPS_FIX) {
        ESP_LOGI(TAG, "Waiting for valid GPS fix before polling workers...");
        while (!gps_fix_acquired) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "GPS Fix acquired! Starting Worker Polling...");
    } else {
        ESP_LOGI(TAG, "Starting Worker Polling (Using last-known NVS GPS if available)...");
    }

    while (1) {
        for (int i = 0; i < 3; i++) {
            ESP_LOGI(TAG, "Polling Worker %d...", i + 1);
            memset(tx, 0, sizeof(controller_msg_t));
            tx->command = HANDSHAKE_CMD;
            tx->wifi_channel = worker_current_channels[i];
            tx->unix_time = current_unix_time; // Send live GPS time
            tx->gps = current_gps;             // Send live coordinates

            spi_transaction_t t = { .length = PAYLOAD_SIZE * 8, .tx_buffer = tx, .rx_buffer = rx };
            if (spi_device_transmit(workers[i], &t) == ESP_OK) {
                if (rx->status == HANDSHAKE_ACK) {
                    uint16_t cal = calculate_checksum(rx->data, sizeof(rx->data));
                    if (rx->checksum == cal) {
                        int count = rx->reserved;
                        ESP_LOGI(TAG, "Worker %d OK. APs: %d", i+1, count);
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
        
        // If GPS isn't fixing, manually increment time for testing
        if (current_gps.latitude == 0.0f) {
            current_unix_time += 15;
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
