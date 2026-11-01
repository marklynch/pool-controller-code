/**
 * Mock esp_crt_bundle.h for host-based testing. The test supplies
 * esp_crt_bundle_attach.
 */

#ifndef ESP_CRT_BUNDLE_H
#define ESP_CRT_BUNDLE_H

#include "esp_err.h"

esp_err_t esp_crt_bundle_attach(void *conf);

#endif // ESP_CRT_BUNDLE_H
