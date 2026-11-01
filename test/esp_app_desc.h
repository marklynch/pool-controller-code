/**
 * Mock esp_app_desc.h for host-based testing. The test supplies
 * esp_app_get_description.
 */

#ifndef ESP_APP_DESC_H
#define ESP_APP_DESC_H

typedef struct {
    char version[32];
} esp_app_desc_t;

const esp_app_desc_t *esp_app_get_description(void);

#endif // ESP_APP_DESC_H
