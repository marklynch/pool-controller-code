#ifndef CONFIG_H
#define CONFIG_H

#define MIN(a, b) ((a) < (b) ? (a) : (b))

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
#define HTTP_MAX_URI_HANDLERS          24      // Number of endpoint handlers
#define HTTP_RECV_TIMEOUT_SEC          10      // Timeout for receiving requests
#define HTTP_SEND_TIMEOUT_SEC          10      // Timeout for sending responses
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
// WiFi reconnection. Stored credentials are the source of truth and are never
// cleared automatically: an AP that is missing or rebooting (power outage, a
// router firmware update, the controller booting faster than the router) says
// nothing about whether they are correct. A device that has credentials retries
// forever, stepping through this backoff table and then repeating its last
// entry, so a multi-hour outage costs almost nothing and heals by itself.
#define WIFI_RETRY_BACKOFF_MS          { 1000, 2000, 5000, 15000, 30000, 60000 }

// Rescue portal. So credentials can still be changed when the network is
// genuinely gone (new router, new password, device moved), the SoftAP is
// brought up *alongside* a station that keeps retrying underneath it, and is
// taken down again the moment the station connects — it is never terminal.
// Repeated authentication failures are positive evidence of stale credentials,
// so they surface the portal sooner than an AP that is merely unreachable.
#define WIFI_RESCUE_PORTAL_DELAY_MS    (15 * 60 * 1000) // Unreachable AP: offline this long
#define WIFI_RESCUE_PORTAL_AUTH_MS     (2 * 60 * 1000)  // Rejected credentials: offline this long
#define WIFI_AUTH_FAIL_THRESHOLD       3                // Consecutive auth failures before the shorter delay applies

// WiFi supervisor task — owns rescue-portal start/stop, since the WiFi mode
// switch must not run in the event-loop task.
#define WIFI_SUPERVISOR_INTERVAL_MS    1000
#define WIFI_SUPERVISOR_STACK          4096
#define WIFI_SUPERVISOR_PRIORITY       4

// How long startup waits for an address before carrying on. Bounded so the bus
// bridge and local services come up whether or not the network does.
#define WIFI_STARTUP_WAIT_MS           15000

// WiFi Scanning
#define WIFI_SCAN_TIME_MIN_MS          100     // Minimum scan time per channel
#define WIFI_SCAN_TIME_MAX_MS          300     // Maximum scan time per channel
#define WIFI_SCAN_MAX_RESULTS          20      // Maximum number of scan results to return
#define WIFI_SCAN_POLL_MS              1000    // Web UI interval between /scan polls while a scan runs
#define WIFI_SCAN_MAX_AGE_MS           30000   // A scan started longer ago than this is stale (or stuck) and is redone

// ======================================================
// Pool Hardware Limits
// ======================================================

#define TEMP_SETPOINT_MIN_C            10      // Minimum allowable setpoint temperature (°C)
#define TEMP_SETPOINT_MAX_C            42      // Maximum allowable setpoint temperature (°C)

#define MAX_CHANNELS                   8       // Maximum number of controllable channels
#define MAX_HEATERS                    2       // Maximum number of heaters
#define MAX_LIGHT_ZONES                8       // Maximum number of light zones (protocol max)
#define MAX_VALVE_SLOTS                2       // Maximum number of valve slots
#define MAX_FAVOURITES                 8       // Pool + Spa (built-ins) + Favourites 1–6 (user)
#define MAX_TIMERS                     16      // Maximum number of timers (registers 0x08-0x17)
#define MAX_REGISTER_LABELS            32      // Maximum number of register label entries
#define MAX_SEEN_DEVICES               16      // Maximum number of distinct source addresses tracked

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

// TCP keepalive — reap a silently dead client (one that never sends FIN/RST)
// so the single client slot is freed. Worst-case detection time is roughly
// IDLE + INTERVAL * COUNT seconds.
#define TCP_KEEPALIVE_IDLE_SEC         30      // Idle seconds before the first keepalive probe
#define TCP_KEEPALIVE_INTERVAL_SEC     5       // Seconds between probes
#define TCP_KEEPALIVE_COUNT            3       // Failed probes before dropping the connection

// ======================================================
// Buffer Sizes
// ======================================================

// Message Buffers
#define BUS_MESSAGE_MAX_SIZE           256     // Maximum bus message size in bytes

// HTTP Response Buffers
#define HTTP_PROVISION_BUFFER_SIZE     512     // Buffer for provisioning requests (SSID 32 + password 63 + JSON overhead)
#define HTTP_MQTT_CONFIG_BUFFER_SIZE   512     // Buffer for MQTT config requests
#define HTTP_WIFI_SCAN_BUFFER_SIZE     4096    // Buffer for WiFi scan results JSON
#define HTTP_OTA_BUFFER_SIZE           4096    // Buffer for OTA firmware chunks

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
#define OTA_MAX_FIRMWARE_SIZE          0x1E0000 // Must match ota_0/ota_1 partition size in partitions.csv

// ======================================================
// GitHub Firmware Update
// ======================================================

// Repository that publishes firmware releases. The latest release is resolved
// via https://github.com/<owner>/<repo>/releases/latest and the OTA image is
// pulled from that release's asset (named <asset_prefix><tag>.bin, matching the
// artifact the build workflow uploads).
#define FW_UPDATE_GITHUB_OWNER         "marklynch"
#define FW_UPDATE_GITHUB_REPO          "pool-controller-code"
#define FW_UPDATE_ASSET_PREFIX         "pool-controller-update-"  // + <tag> + ".bin"

#define FW_UPDATE_CHECK_INTERVAL_MS    (12 * 60 * 60 * 1000)  // Re-check GitHub every 12 hours
#define FW_UPDATE_STARTUP_DELAY_MS     30000    // Wait after boot before the first check
#define FW_UPDATE_HTTP_TIMEOUT_MS      15000    // Per-request timeout for the version check
#define FW_UPDATE_OTA_TIMEOUT_MS       60000    // Socket timeout while downloading the image
#define FW_UPDATE_HTTP_BUF_SIZE        2048     // GitHub redirects the asset download to a
                                                // signed URL well over the 512-byte default;
                                                // needed for both the Location header (RX)
                                                // and the redirected request line (TX)
#define FW_UPDATE_TASK_STACK           8192     // Stack for the check/install task (TLS is heavy)

// Number of most-recent releases to track (latest + 4 prior), so the web UI
// can offer installing/rolling back to a specific version.
#define FW_UPDATE_MAX_VERSIONS         5
#define FW_UPDATE_VERSION_LEN          48       // Max length of a version tag string

#endif // CONFIG_H
