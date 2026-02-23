#ifndef CONFIG_H
#define CONFIG_H

// ======================================================
// Network Configuration
// ======================================================

// mDNS Service Discovery
#define MDNS_HOSTNAME                  "poolcontrol"           // Hostname prefix; full hostname is poolcontrol-AABBCC.local
#define MDNS_INSTANCE_NAME             "Pool Control"          // Friendly name for HTTP service (suffix AABBCC appended at runtime)
#define MDNS_INSTANCE_DEBUG_NAME       "Pool Control Debug"    // Friendly name for TCP bridge service (suffix AABBCC appended at runtime)

// Device Identity (used in MQTT discovery)
#define DEVICE_NAME                    "Pool Controller"
#define DEVICE_MODEL                   "ESP32-C6 Bridge"
#define DEVICE_MANUFACTURER            "Mark Lynch"

// HTTP Server
#define HTTP_SERVER_PORT               80
#define HTTP_MAX_URI_HANDLERS          14      // Number of endpoint handlers
#define HTTP_RECV_TIMEOUT_SEC          60      // Timeout for large file uploads
#define HTTP_SEND_TIMEOUT_SEC          60      // Timeout for responses
#define HTTP_STACK_SIZE                8192    // Stack size for HTTP server task

// TCP Bridge
#define TCP_BRIDGE_PORT                7373

// MQTT
#define MQTT_DEFAULT_PORT              1883    // Default MQTT broker port

// NTP Time Sync
#define NTP_SERVER                     "pool.ntp.org"  // NTP server for time synchronization

// DNS Server (captive portal)
#define DNS_PORT                       53      // DNS server port
#define DNS_MAX_PACKET_SIZE            512     // Maximum DNS packet size
#define DNS_TASK_STACK_SIZE            4096    // DNS server task stack size
#define DNS_TASK_PRIORITY              5       // DNS server task priority

// WiFi Provisioning
#define WIFI_PROV_SOFTAP_IP            "192.168.4.1"
#define WIFI_PROV_SOFTAP_PASSWORD      "poolsetup"     // Default password for provisioning AP
#define WIFI_PROV_SOFTAP_SSID_PREFIX   "POOL_"         // Prefix for SoftAP SSID (followed by MAC address)
#define WIFI_PROV_NVS_NAMESPACE        "wifi_config"   // NVS namespace for WiFi credentials
#define WIFI_PROV_NVS_KEY_SSID         "ssid"          // NVS key for SSID
#define WIFI_PROV_NVS_KEY_PASS         "password"      // NVS key for password
#define WIFI_MAX_RETRY_ATTEMPTS        5               // Max connection attempts before clearing credentials
#define WIFI_RETRY_DELAY_MS            5000            // Delay between retry attempts
#define WIFI_RESTART_DELAY_MS          1000            // Delay before restart after clearing credentials

// WiFi Scanning
#define WIFI_SCAN_TIME_MIN_MS          100     // Minimum scan time per channel
#define WIFI_SCAN_TIME_MAX_MS          300     // Maximum scan time per channel
#define WIFI_SCAN_MAX_RESULTS          20      // Maximum number of scan results to return

// ======================================================
// Pool Hardware Limits
// ======================================================

#define MAX_CHANNELS                   8       // Maximum number of controllable channels
#define MAX_LIGHT_ZONES                4       // Maximum number of light zones
#define MAX_TIMERS                     16      // Maximum number of timers (registers 0x08-0x17)

// ======================================================
// UART/Bus Configuration
// ======================================================

// Hardware Configuration
#define BUS_UART_NUM                   UART_NUM_1  // UART port number
#define BUS_BAUD_RATE                  9600        // Baud rate (change to match your bus protocol)
#define BUS_TX_GPIO                    2           // TX GPIO (GPIO2 -> NPN base via 10k resistor)
#define BUS_RX_GPIO                    1           // RX GPIO (GPIO1 <- voltage divider tap)

// UART Buffers
#define UART_RX_BUFFER_SIZE            2048
#define UART_TX_BUFFER_SIZE            2048
#define UART_RX_TIMEOUT_MS             15     // Timeout waiting for RX read (at 9600 baud, ~1ms per byte)
#define UART_TX_TIMEOUT_MS             100     // Timeout waiting for TX completion

// ======================================================
// LED Configuration
// ======================================================

#define LED_GPIO                       8       // WS2812 RGB LED GPIO pin
#define LED_FLASH_DURATION_MS          50      // Flash duration in milliseconds

// ======================================================
// TCP Bridge Configuration
// ======================================================

#define TCP_UART_BUFFER_SIZE           64     // UART read buffer for TCP bridge - large enough for typical bus messages
#define TCP_BUFFER_SIZE                256     // TCP socket buffer size
#define TCP_LINE_BUFFER_SIZE           512     // Line buffering for TCP bridge
#define TCP_TASK_STACK_SIZE            8192    // TCP bridge task stack size
#define TCP_TASK_PRIORITY              5       // TCP bridge task priority

// ======================================================
// Buffer Sizes
// ======================================================

// Message Buffers
#define BUS_MESSAGE_MAX_SIZE           256     // Maximum bus message size in bytes

// HTTP Response Buffers
#define HTTP_PROVISION_BUFFER_SIZE     200     // Buffer for provisioning requests
#define HTTP_MQTT_CONFIG_BUFFER_SIZE   512     // Buffer for MQTT config requests
#define HTTP_WIFI_SCAN_BUFFER_SIZE     4096    // Buffer for WiFi scan results JSON
#define HTTP_STATUS_BUFFER_SIZE        8192    // Buffer for pool status JSON
#define HTTP_OTA_BUFFER_SIZE           4096    // Buffer for OTA firmware chunks

// MQTT Buffers
#define MQTT_DISCOVERY_CONFIG_SIZE     1024    // Buffer for MQTT discovery config messages

// ======================================================
// Timeouts & Delays
// ======================================================

// General Timeouts
#define MUTEX_TIMEOUT_MS               100     // Timeout for acquiring mutexes
#define TASK_DELAY_MS                  1000    // Standard task delay
#define LOOPBACK_DETECTION_MS          500     // Window to detect echo of own TX messages on the bus

// OTA Update
#define OTA_REBOOT_DELAY_MS            2000    // Delay before reboot after OTA
#define OTA_UPLOAD_TIMEOUT_MS          120000  // 2 minutes max for OTA upload

#endif // CONFIG_H
