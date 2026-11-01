# OTA (Over-The-Air) Firmware Updates

This document describes the OTA firmware update features.

## Overview

The device supports updating firmware over WiFi without needing a USB connection. The OTA update system uses dual app partitions for safe rollback if the new firmware fails to boot.

There are three ways to update:

1. **Update from GitHub (recommended)** — the device checks the project's GitHub *latest release* on a schedule, and can download and install it on demand. No file handling required.
2. **Home Assistant update entity** — the "Firmware" update entity mirrors the GitHub check and installs with the HA *Install* button.
3. **Manual upload** — upload a locally built `.bin` from the `/update` page.

## Update from GitHub

### How it works

- On boot (after a short delay) and every `FW_UPDATE_CHECK_INTERVAL_S` (default 12 h), the firmware fetches the releases Atom feed `https://github.com/<owner>/<repo>/releases.atom` and reads the tags of the most recent releases (newest first) — the latest plus up to `FW_UPDATE_MAX_VERSIONS - 1` prior versions (default: latest + 4). This uses no GitHub API token and no rate-limited JSON call. The feed is parsed as it streams, so a repo with large release notes doesn't need a big buffer.
- The newest tag is compared against the running build's version (`major.minor.patch`, ignoring any `git describe` suffix). If it is newer, an update is flagged as available.
- Installing pulls the chosen release's asset `pool-controller-update-<tag>.bin` (the artifact the build workflow uploads) directly over HTTPS via `esp_https_ota`, writing to the inactive OTA partition, then reboots. The requested version must be one of the recently discovered releases, so the download URL can't be pointed at an arbitrary target.

The GitHub owner/repo, asset name prefix, number of versions tracked, check interval, and timeouts are configured in `main/config.h` (`FW_UPDATE_*`).

### From the web UI

1. Open `http://<device-ip>/update`.
2. The **Update from GitHub** section shows whether you are up to date or a newer release is available (with a link to the release notes).
3. Choose a version from the dropdown — the latest plus the previous few releases, so you can update or **roll back** to a specific version — then click **Install selected version**. Click **Check for updates** to re-query GitHub immediately. Progress is shown; the device reboots when finished.
4. Untick **Automatically check for new releases** to stop the 12-hourly check. **Check for updates** (and Home Assistant's check button) still work while it's off. The setting is remembered across reboots and updates.

> Rolling back requires that the older release still has a `pool-controller-update-<tag>.bin` asset attached; releases published before this asset naming was in place cannot be installed this way.

### From Home Assistant

With MQTT configured, the device publishes a Home Assistant `update` entity ("Firmware") via MQTT discovery. It shows the installed and latest versions and provides an **Install** button that triggers the same GitHub OTA. Download progress is reported back to the entity.

### Network requirements

The device must be able to reach `github.com` and `objects.githubusercontent.com` (where release downloads redirect) over HTTPS. TLS is validated against the ESP-IDF certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`).

## Partition Layout

The device uses the following partition table (defined in `partitions.csv`):

- **nvs** (0x6000 / 24KB) - Non-volatile storage for WiFi credentials, settings
- **phy_init** (0x1000 / 4KB) - PHY initialization data
- **otadata** (0x2000 / 8KB) - OTA data partition (tracks which OTA partition to boot)
- **factory** (0x180000 / 1.5MB) - Initial firmware partition
- **ota_0** (0x180000 / 1.5MB) - First OTA update partition
- **ota_1** (0x180000 / 1.5MB) - Second OTA update partition

Updates alternate between ota_0 and ota_1, providing automatic rollback protection. The **otadata** partition is critical - it stores which OTA partition should boot next.

## Manual Upload (build and upload a .bin)

Use this when developing locally or installing a build that isn't a published GitHub release.

### 1. Build New Firmware

```bash
idf.py build
```

The firmware binary will be at: `build/pool-controller.bin`

### 2. Access Update Page

1. Connect to the device's WiFi network or ensure you're on the same network
2. Navigate to: `http://<device-ip>/update`
3. The page shows:
   - Current firmware version
   - Partition that will be written
   - File upload form

### 3. Upload Firmware

1. Click "Select Firmware File (.bin)"
2. Choose the `pool-controller.bin` file from the build directory
3. Click "Upload and Update"
4. **DO NOT power off the device during update!**

### 4. Monitor Progress

- Upload progress bar shows transfer status
- Success message appears when update completes
- Device automatically restarts with new firmware

## Safety Features

### Automatic Rollback

If the new firmware fails to boot (crashes, bootloops, etc.), the ESP32 automatically reverts to the previous working firmware after 3 failed boot attempts.

### Boot Confirmation

The new firmware must call `esp_ota_mark_app_valid_cancel_rollback()` within the first few boots to confirm it's working. This is handled automatically in `main.c`.

### Validation

- Firmware image is validated before writing (checksums, format verification)
- Invalid images are rejected before any changes are made
- Partition integrity is verified after writing

## Troubleshooting

### "No OTA partition configured" Error

Ensure `partitions.csv` is properly configured in the build system. Check that menuconfig uses the custom partition table:
```bash
idf.py menuconfig
# Navigate to: Partition Table -> Partition Table -> Custom partition table CSV
# Set: partitions.csv
```

### Update Fails Midway

- Check WiFi signal strength (update can fail on poor connection)
- Ensure the .bin file is not corrupted (re-build if necessary)
- Verify sufficient free space on update partition

### Device Won't Boot After Update

The device should automatically rollback. If it doesn't:
1. Connect via USB serial
2. Check logs with `idf.py monitor`
3. Manually flash factory firmware if needed: `idf.py flash`

### Version Shows as "dirty"

Commit your Git changes before building:
```bash
git commit -m "Your commit message"
idf.py build
```

The version is generated from Git tags using `git describe`.

## Security Considerations

### Current Implementation

- Web upload requires network access to device
- No authentication on /update endpoint (rely on network security)
- No firmware signature verification

### Recommended Improvements for Production

1. **Add authentication** - Require password before allowing upload
2. **Use HTTPS** - Encrypt firmware transfer
3. **Enable signature verification** - ESP-IDF supports signed OTA images
4. **Rate limiting** - Prevent brute force attempts

Example of enabling signature verification in menuconfig:
```
Security Features -> Enable hardware Secure Boot in bootloader
```

## Version Information

The current firmware version is displayed:
- On the `/update` page
- In the `/status` API endpoint
- In boot logs via serial monitor

Version format: `v{tag}-{commits}-g{hash}[-dirty]`
- Example: `v1.0.0-5-g870d65b` = 5 commits after v1.0.0 tag

The GitHub install endpoints (`POST /update/github`, `POST /update`) and the MQTT install command likewise have no authentication and rely on network/broker security. The GitHub image download is transport-encrypted (HTTPS, validated against the certificate bundle) but the image itself is not signature-verified unless Secure Boot / signed OTA is enabled.

## Files

- `partitions.csv` - Partition table definition
- `main/firmware_update.c/.h` - GitHub release check + pull-based OTA
- `main/web_handlers.c` - Manual upload handler + `/update/check` and `/update/github` endpoints
- `main/mqtt_discovery.c` - Home Assistant `update` entity discovery
- `main/mqtt_poolclient.c` - Subscribes to and routes the MQTT install command
- `main/config.h` - `FW_UPDATE_*` settings (repo, interval, timeouts)
- `main/CMakeLists.txt` - `esp_https_ota` / `esp_http_client` / `esp-tls` / `mbedtls` dependencies
- `sdkconfig.defaults` - Enables the mbedTLS certificate bundle

## References

- [ESP-IDF OTA Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ota.html)
- [ESP HTTPS OTA Component](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/esp_https_ota.html)
- [app_update Component](https://github.com/espressif/esp-idf/tree/master/components/app_update)
- [Home Assistant MQTT Update](https://www.home-assistant.io/integrations/update.mqtt/)
