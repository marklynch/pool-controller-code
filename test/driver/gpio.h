#ifndef DRIVER_GPIO_H
#define DRIVER_GPIO_H

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

#define GPIO_MODE_OUTPUT          1
#define GPIO_PULLUP_DISABLE       0
#define GPIO_PULLDOWN_DISABLE     0
#define GPIO_INTR_DISABLE         0

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(int gpio_num, uint32_t level);

#endif
