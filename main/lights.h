#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"

typedef struct {
    uint8_t button_gpio;
    uint8_t light_gpio;
    uint8_t led_gpio;
    bool is_dimmer;
} lights_t;

#ifdef __cplusplus
}
#endif 