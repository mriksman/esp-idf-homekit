#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    button_active_low = 0,
    button_active_high = 1,
} button_active_level_t;

typedef struct {
    button_active_level_t active_level;
    // times in milliseconds
    uint16_t repeat_press_timeout;
    uint16_t long_press_time;
} button_config_t;

typedef enum {
    button_event_down = -3,
    button_event_up = -2,
    button_event_long_press = -1,
} button_event_t;

typedef void (*button_callback_fn)(button_event_t event, void* context);

#define BUTTON_CONFIG(level, ...) \
  (button_config_t) { \
    .active_level = level, \
    .repeat_press_timeout = 300, \
    __VA_ARGS__ \
  }

int button_create(uint8_t gpio_num,
                  button_config_t config,
                  button_callback_fn callback,
                  void* context);

void button_destroy(uint8_t gpio_num);

#ifdef __cplusplus
}
#endif 