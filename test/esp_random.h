/**
 * Mock esp_random.h for host-based testing. The test supplies esp_fill_random.
 */

#ifndef ESP_RANDOM_H
#define ESP_RANDOM_H

#include <stddef.h>

void esp_fill_random(void *buf, size_t len);

#endif // ESP_RANDOM_H
