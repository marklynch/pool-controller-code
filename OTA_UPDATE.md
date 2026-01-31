# OTA (Over-The-Air) Firmware Updates

This document describes how to use the web-based OTA firmware update feature.

## Overview

The device supports updating firmware over WiFi without needing a USB connection. The OTA update system uses dual app partitions for safe rollback if the new firmware fails to boot.

## Partition Layout

The device uses the following partition table (defined in `partitions.csv`):

- **factory** (0x180000 / 1.5MB) - Initial firmware partition
- **ota_0** (0x180000 / 1.5MB) - First OTA update partition
- **ota_1** (0x180000 / 1.5MB) - Second OTA update partition

Updates alternate between ota_0 and ota_1, providing automatic rollback protection.

## How to Update Firmware

### 1. Build New Firmware

```bash
idf.py build
```

The firmware binary will be at: `build/astral-pool-controller.bin`

### 2. Access Update Page

1. Connect to the device's WiFi network or ensure you're on the same network
2. Navigate to: `http://<device-ip>/update`
3. The page shows:
   - Current firmware version
   - Partition that will be written
   - File upload form

### 3. Upload Firmware

1. Click "Select Firmware File (.bin)"
2. Choose the `astral-pool-controller.bin` file from the build directory
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

## Files Modified

- `partitions.csv` - Partition table definition
- `main/web_handlers.c` - OTA upload handlers
- `main/CMakeLists.txt` - Added app_update component dependency
- `CMakeLists.txt` - (No changes needed for basic OTA)

## References

- [ESP-IDF OTA Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ota.html)
- [app_update Component](https://github.com/espressif/esp-idf/tree/master/components/app_update)
