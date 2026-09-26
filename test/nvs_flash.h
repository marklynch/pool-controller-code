/**
 * Mock nvs_flash.h for host-based testing — the real header is separate from
 * nvs.h but the stub needs nothing extra, so just forward to it.
 */

#ifndef NVS_FLASH_H
#define NVS_FLASH_H

#include "nvs.h"

#endif // NVS_FLASH_H
