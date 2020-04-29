#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_ACTIVE_LOW = 0,
    BUTTON_ACTIVE_HIGH = 1,
} button_active_level_t;

typedef struct {
    button_active_level_t active_level;
    // times in milliseconds
    uint16_t repeat_press_timeout;
    uint16_t long_press_time;
} button_config_t;

typedef enum {
    BUTTON_EVENT_DOWN = 100,
    BUTTON_EVENT_UP = 101,
    BUTTON_EVENT_DOWN_HOLD = 102,
    BUTTON_EVENT_UP_HOLD = 103,
    BUTTON_EVENT_LONG_PRESS = 104,    // Can't use -1 as that is what ESP_EVENT_ANY_ID is
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