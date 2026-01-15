#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "led_strip.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include "lwip/netdb.h"
#include "lwip/err.h"

// ==================== USER CONFIG =====================

#include "secrets.h"  // WIFI_SSID, WIFI_PASS

#define TCP_PORT        7373

// UART/bus config
#define BUS_UART_NUM    UART_NUM_1
#define BUS_BAUD_RATE   9600        // change to match bus
#define BUS_TX_GPIO     2           // GPIO2 -> NPN base (via 10k)
#define BUS_RX_GPIO     1           // GPIO1 <- divider tap

// UART buffer sizes
#define UART_RX_BUF_SIZE    (2048)
#define UART_TX_BUF_SIZE    (2048)

// RGB LED config (WS2812 on GPIO8)
#define LED_GPIO            8
#define LED_FLASH_MS        50  // flash duration in milliseconds

// Wi-Fi event bits
#define WIFI_CONNECTED_BIT  BIT0

static const char *TAG = "POOL_BUS_BRIDGE";

static EventGroupHandle_t s_wifi_event_group;
static led_strip_handle_t s_led_strip;

// ======================================================
// Wi-Fi event handler
// ======================================================

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ======================================================
// RGB LED init and helpers
// ======================================================

static void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip));
    led_strip_clear(s_led_strip);

    ESP_LOGI(TAG, "RGB LED initialised on GPIO %d", LED_GPIO);
}


static void led_flash_blue(void)
{
    led_strip_set_pixel(s_led_strip, 0, 0, 0, 32);  // Blue (R=0, G=0, B=32)
    led_strip_refresh(s_led_strip);
    vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
    led_strip_clear(s_led_strip);
    led_strip_refresh(s_led_strip);
}

// Flash green when receiving bytes from the bus (UART RX)
static void led_flash_rx(void)
{
    led_strip_set_pixel(s_led_strip, 0, 32, 0, 0);  // Red (R=32, G=0, B=0)
    led_strip_refresh(s_led_strip);
    vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
    led_strip_clear(s_led_strip);
    led_strip_refresh(s_led_strip);
}

// Flash red when sending bytes to the bus(UART TX)
static void led_flash_tx(void)
{
    led_strip_set_pixel(s_led_strip, 0, 0, 32, 0);  // Green (R=0, G=32, B=0)
    led_strip_refresh(s_led_strip);
    vTaskDelay(pdMS_TO_TICKS(LED_FLASH_MS));
    led_strip_clear(s_led_strip);
    led_strip_refresh(s_led_strip);
}

// ======================================================
// Message decoder
// ======================================================

// Address Subsystem
// 50 Main Controller (Connect 10)
// 62 Temperature sensor
// 90 Chemistry (pH, ORP, Chlorinator)
// 6F Touch Screen (source only) 

// Assumption of message structure is:
// 02 Start Byte
// 00 50 - Source Address (example: 50 = Main Controller)
// FF FF - Destination Address (FF FF = Broadcast)
// 80 00 Control Byte or Flags
// 12 - command
// 0D - sub-command or length
// 2F - register/parameter
// 01 - value
// 01 - value or checksum component
// 03 End Byte

// Message type patterns (messages start with 0x02, end with 0x03)
// 50 Main Controller (Connect 10)
static const uint8_t MSG_TYPE_38[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x38, 0x0F};
static const uint8_t MSG_TYPE_TEMP_SETTING[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x17, 0x10};
static const uint8_t MSG_TYPE_CONFIG[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x26, 0x0E};
static const uint8_t MSG_TYPE_MODE[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x14, 0x0D, 0xF1};
static const uint8_t MSG_TYPE_CHANNELS[] = {0x02, 0x00, 0x50, 0x00, 0x6F, 0x80, 0x00, 0x0D, 0x0D, 0x5B};
static const uint8_t MSG_TYPE_CHANNEL_STATUS[] = {0x02, 0x00, 0x50, 0xFF, 0xFF, 0x80, 0x00, 0x0B, 0x25, 0x00};

// 62 Temperature sensor / Unknown subsystem
static const uint8_t MSG_TYPE_TEMP_READING[] = {0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00, 0x16, 0x0E};
static const uint8_t MSG_TYPE_HEATER[] = {0x02, 0x00, 0x62, 0xFF, 0xFF, 0x80, 0x00, 0x12, 0x0F};

#define MSG_HEATER_STATE_IDX 11

// 90 Chemistry (pH, ORP, Chlorinator)
static const uint8_t MSG_TYPE_CHLOR[] = {0x02, 0x00, 0x90, 0xFF, 0xFF, 0x80, 0x00};

// 6F Touch Screen (source only)


// Channel type names
static const char *CHANNEL_TYPE_NAMES[] = {
    "Unknown",       // 0
    "Filter",        // 1
    "Cleaning",      // 2
    "Heater Pump",   // 3
    "Booster",       // 4
    "Waterfall",     // 5
    "Fountain",      // 6
    "Spa Pump",      // 7
    "Solar",         // 8
    "Blower",        // 9
    "Swimjet",       // 10
    "Jets",          // 11
    "Spa Jets",      // 12
    "Overflow",      // 13
    "Spillway",      // 14
    "Audio",         // 15
    "Hot Seat",      // 16
    "Heater Power",  // 17
    "Custom Name",   // 18
};
#define CHANNEL_TYPE_COUNT 19
#define CHANNEL_UNUSED 0xFE
#define CHANNEL_END    0xFD

// Channel state names
static const char *CHANNEL_STATE_NAMES[] = {
    "Off",          // 0
    "Auto",         // 1
    "On",           // 2
    "Low Speed",    // 3
    "Medium Speed", // 4
    "High Speed",   // 5
};
#define CHANNEL_STATE_COUNT 6

// Lighting state names
static const char *LIGHTING_STATE_NAMES[] = {
    "Off",          // 0
    "Auto",         // 1
    "On",           // 2
};
#define LIGHTING_STATE_COUNT 3

// Byte offset for temperature scale in MSG_TYPE_CONFIG
#define MSG_CONFIG_TEMP_SCALE_IDX  10

// Byte offset for pool/spa mode
#define MSG_MODE_IDX  10

// Chlorinator sub-type patterns (bytes 7-10)
static const uint8_t CHLOR_PH_SETPOINT[]  = {0x1D, 0x0F, 0x3C, 0x01};
static const uint8_t CHLOR_ORP_SETPOINT[] = {0x1D, 0x0F, 0x3C, 0x02};
static const uint8_t CHLOR_PH_READING[]   = {0x1F, 0x0F, 0x3E, 0x01};
static const uint8_t CHLOR_ORP_READING[]  = {0x1F, 0x0F, 0x3E, 0x02};

// Byte offsets for MSG_TYPE_TEMP_SETTING (temperature settings)
#define MSG_17_SPA_SET_TEMP_IDX   10
#define MSG_17_POOL_SET_TEMP_IDX  11

// Byte offsets for MSG_TYPE_TEMP_READING (current temperature)
#define MSG_16_CURRENT_TEMP_IDX   10

// Returns true if message was decoded, false otherwise
static bool decode_message(const uint8_t *data, int len)
{
    // Must start with 0x02 and end with 0x03
    if (len < 2 || data[0] != 0x02 || data[len - 1] != 0x03) {
        return false;
    }

    if (len >= sizeof(MSG_TYPE_38) && memcmp(data, MSG_TYPE_38, sizeof(MSG_TYPE_38)) == 0) {
        uint8_t channel = data[10];
        uint8_t value1 = data[11];
        uint8_t value2 = data[12];

        if ((channel & 0xF0) == 0xC0) {
            // Lighting zone
            const char *state = (value2 < LIGHTING_STATE_COUNT) ? LIGHTING_STATE_NAMES[value2] : "Unknown";
            ESP_LOGI(TAG, "MSG: Lighting zone 0x%02X - %s", channel, state);
        } else {
            ESP_LOGI(TAG, "MSG: Type 0x38 - Channel=0x%02X, value1=%d, value2=%d", channel, value1, value2);
        }
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_CONFIG) && memcmp(data, MSG_TYPE_CONFIG, sizeof(MSG_TYPE_CONFIG)) == 0) {
        uint8_t config_byte = data[MSG_CONFIG_TEMP_SCALE_IDX];
        const char *scale_str = (config_byte & 0x10) ? "Fahrenheit" : "Celsius";
        ESP_LOGI(TAG, "MSG: Config - temperature scale=%s", scale_str);
        return false;
    }
    else if (len >= sizeof(MSG_TYPE_MODE) && memcmp(data, MSG_TYPE_MODE, sizeof(MSG_TYPE_MODE)) == 0) {
        uint8_t mode = data[MSG_MODE_IDX];
        const char *mode_str = (mode == 0x00) ? "Spa" : (mode == 0x01) ? "Pool" : "Unknown";
        ESP_LOGI(TAG, "MSG: Mode - %s", mode_str);
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_CHANNELS) + 2 && memcmp(data, MSG_TYPE_CHANNELS, sizeof(MSG_TYPE_CHANNELS)) == 0) {
        uint8_t bitmask = data[10];
        ESP_LOGI(TAG, "MSG: Active channels - 0x%02X [%c%c%c%c%c%c%c%c]",
                 bitmask,
                 (bitmask & 0x80) ? '8' : '-',
                 (bitmask & 0x40) ? '7' : '-',
                 (bitmask & 0x20) ? '6' : '-',
                 (bitmask & 0x10) ? '5' : '-',
                 (bitmask & 0x08) ? '4' : '-',
                 (bitmask & 0x04) ? '3' : '-',
                 (bitmask & 0x02) ? '2' : '-',
                 (bitmask & 0x01) ? '1' : '-');
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_CHANNEL_STATUS) + 1 && memcmp(data, MSG_TYPE_CHANNEL_STATUS, sizeof(MSG_TYPE_CHANNEL_STATUS)) == 0) {
        uint8_t num_channels = data[10];
        ESP_LOGI(TAG, "MSG: Channel status (%d channels):", num_channels);
        int idx = 11;  // Channel data starts after header
        int ch_num = 1;
        bool past_end = false;
        while (ch_num <= num_channels) {
            if (past_end || idx + 2 >= len - 1) {
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                ch_num++;
                continue;
            }
            uint8_t ch_type = data[idx];
            if (ch_type == CHANNEL_END) {
                past_end = true;
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                ch_num++;
                continue;
            }
            uint8_t state = data[idx + 1];
            const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";

            if (ch_type == CHANNEL_UNUSED) {
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
            } else {
                const char *type_name = (ch_type < CHANNEL_TYPE_COUNT) ? CHANNEL_TYPE_NAMES[ch_type] : "Unknown";
                ESP_LOGI(TAG, "  Ch%d: %s (%d) = %s", ch_num, type_name, ch_type, state_name);
            }
            idx += 3;
            ch_num++;
        }
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_TEMP_SETTING) && memcmp(data, MSG_TYPE_TEMP_SETTING, sizeof(MSG_TYPE_TEMP_SETTING)) == 0) {
        uint8_t spa_set_temp = data[MSG_17_SPA_SET_TEMP_IDX];
        uint8_t pool_set_temp = data[MSG_17_POOL_SET_TEMP_IDX];
        ESP_LOGI(TAG, "MSG: Temperature settings - spa_set_temp=%d, pool_set_temp=%d", spa_set_temp, pool_set_temp);
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_TEMP_READING) && memcmp(data, MSG_TYPE_TEMP_READING, sizeof(MSG_TYPE_TEMP_READING)) == 0) {
        uint8_t current_temp = data[MSG_16_CURRENT_TEMP_IDX];
        ESP_LOGI(TAG, "MSG: Current temperature - current_temp=%d", current_temp);
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_HEATER) && memcmp(data, MSG_TYPE_HEATER, sizeof(MSG_TYPE_HEATER)) == 0) {
        uint8_t heater_state = data[MSG_HEATER_STATE_IDX];
        ESP_LOGI(TAG, "MSG: Heater - %s", heater_state ? "On" : "Off");
        return true;
    }
    else if (len >= sizeof(MSG_TYPE_CHLOR) + 6 && memcmp(data, MSG_TYPE_CHLOR, sizeof(MSG_TYPE_CHLOR)) == 0) {
        const uint8_t *sub = &data[7];
        uint16_t value = data[11] | (data[12] << 8);  // little endian

        if (memcmp(sub, CHLOR_PH_SETPOINT, sizeof(CHLOR_PH_SETPOINT)) == 0) {
            ESP_LOGI(TAG, "MSG: Chlorinator pH setpoint - %.1f", value / 10.0);
            return true;
        }
        else if (memcmp(sub, CHLOR_ORP_SETPOINT, sizeof(CHLOR_ORP_SETPOINT)) == 0) {
            ESP_LOGI(TAG, "MSG: Chlorinator ORP setpoint - %d mV", value);
            return true;
        }
        else if (memcmp(sub, CHLOR_PH_READING, sizeof(CHLOR_PH_READING)) == 0) {
            ESP_LOGI(TAG, "MSG: Chlorinator pH reading - %.1f", value / 10.0);
            return true;
        }
        else if (memcmp(sub, CHLOR_ORP_READING, sizeof(CHLOR_ORP_READING)) == 0) {
            ESP_LOGI(TAG, "MSG: Chlorinator ORP reading - %d mV", value);
            return true;
        }
    }

    return false;
}

// ======================================================
// Wi-Fi station init
// ======================================================

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT,
                                   ESP_EVENT_ANY_ID,
                                   &wifi_event_handler,
                                   NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT,
                                   IP_EVENT_STA_GOT_IP,
                                   &wifi_event_handler,
                                   NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init STA complete, connecting...");
}

// ======================================================
// UART (bus) init
// ======================================================

static void uart_bus_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = BUS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(BUS_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(BUS_UART_NUM,
                                 BUS_TX_GPIO,
                                 BUS_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    // Install UART driver
    ESP_ERROR_CHECK(uart_driver_install(BUS_UART_NUM,
                                        UART_RX_BUF_SIZE,
                                        UART_TX_BUF_SIZE,
                                        0,
                                        NULL,
                                        0));

    // Invert TX so:
    //   - UART peripheral TX is inverted once,
    //   - transistor inverts again,
    //   => bus sees normal idle-high UART.
    ESP_ERROR_CHECK(uart_set_line_inverse(BUS_UART_NUM, UART_SIGNAL_TXD_INV));

    ESP_LOGI(TAG, "Bus UART initialised: UART%d, RX=%d, TX=%d, baud=%d",
             BUS_UART_NUM, BUS_RX_GPIO, BUS_TX_GPIO, BUS_BAUD_RATE);
}

// ======================================================
// TCP server task: bridge UART <-> TCP client
// ======================================================

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int listen_sock = -1;
    int client_sock = -1;
    uint8_t uart_buf[256];
    uint8_t tcp_buf[256];

    // Create listening socket
    while (listen_sock < 0) {
        struct sockaddr_in listen_addr = {0};
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_addr.sin_port = htons(TCP_PORT);

        listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(listen_sock, (struct sockaddr *)&listen_addr,
                 sizeof(listen_addr)) < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (listen(listen_sock, 1) < 0) {
            ESP_LOGE(TAG, "Error during listen: errno %d", errno);
            close(listen_sock);
            listen_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Make listening socket non-blocking
        int flags = fcntl(listen_sock, F_GETFL, 0);
        fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK);

        ESP_LOGI(TAG, "TCP server listening on port %d", TCP_PORT);
    }

    // Main loop - always reads UART, optionally bridges to TCP client
    while (1) {
        // Check for new client connection (non-blocking)
        if (client_sock < 0) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            client_sock = accept(listen_sock,
                                 (struct sockaddr *)&client_addr,
                                 &addr_len);
            if (client_sock >= 0) {
                inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
                ESP_LOGI(TAG, "Client connected from %s", addr_str);

                const char *hello =
                    "Connected to ESP32-C6 pool bus bridge.\r\n"
                    "UART bytes will be shown here in hex.\r\n"
                    "Bytes you send will be forwarded to the bus.\r\n\r\n";
                send(client_sock, hello, strlen(hello), 0);
            }
        }

        // 1. UART RX - always read and log
        int len = uart_read_bytes(BUS_UART_NUM,
                                  uart_buf,
                                  sizeof(uart_buf),
                                  10 / portTICK_PERIOD_MS); // 10 ms
        if (len > 0) {
            led_flash_rx();

            // Format as hex string
            char hexLine[3 * sizeof(uart_buf) + 4];
            int pos = 0;
            for (int i = 0; i < len; ++i) {
                if (pos < (int)(sizeof(hexLine) - 4)) {
                    pos += snprintf(&hexLine[pos],
                                    sizeof(hexLine) - pos,
                                    "%02X ",
                                    uart_buf[i]);
                }
            }
            hexLine[pos] = '\0';

            // Decode the message, log hex only if not decoded
            if (!decode_message(uart_buf, len)) {
                ESP_LOGI(TAG, "RX: %s", hexLine);
            }

            // Send to client if connected
            if (client_sock >= 0) {
                hexLine[pos++] = '\r';
                hexLine[pos++] = '\n';
                int sent = send(client_sock, hexLine, pos, 0);
                if (sent < 0) {
                    ESP_LOGW(TAG, "Client send error: errno %d", errno);
                    shutdown(client_sock, SHUT_RDWR);
                    close(client_sock);
                    client_sock = -1;
                }
            }
        }

        // 2. TCP -> UART (only if client connected)
        if (client_sock >= 0) {
            int r = recv(client_sock,
                         tcp_buf,
                         sizeof(tcp_buf),
                         MSG_DONTWAIT);
            if (r > 0) {
                int written = uart_write_bytes(BUS_UART_NUM,
                                               (const char *)tcp_buf,
                                               r);
                ESP_LOGD(TAG, "TCP->UART: wrote %d bytes", written);
                led_flash_tx();
            } else if (r == 0) {
                ESP_LOGI(TAG, "Client disconnected");
                shutdown(client_sock, SHUT_RDWR);
                close(client_sock);
                client_sock = -1;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "Client recv error: errno %d", errno);
                    shutdown(client_sock, SHUT_RDWR);
                    close(client_sock);
                    client_sock = -1;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ======================================================
// app_main
// ======================================================

void app_main(void)
{
    // Init NVS (required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    
    wifi_init_sta();
    uart_bus_init();
    led_init();
    led_flash_blue();  // Flash blue on startup

    // Wait for Wi-Fi connection
    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdFALSE,
                        portMAX_DELAY);

    ESP_LOGI(TAG, "Starting TCP server task...");
    xTaskCreate(tcp_server_task,
                "tcp_server_task",
                4096,
                NULL,
                5,
                NULL);
}