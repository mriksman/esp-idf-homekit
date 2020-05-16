#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>
    
#include <string.h>
#include <driver/gpio.h>

#include "button.h"

#define MAX_TOGGLE_VALUE 4
#define MIN(a, b) (((b) < (a)) ? (b) : (a))
#define MAX(a, b) (((a) < (b)) ? (b) : (a))

typedef struct _button {
    uint8_t gpio_num;
    button_config_t config;
    button_callback_fn callback;
    void* context;

    int8_t value;
    bool last_high;

    uint8_t press_count;
    TimerHandle_t repeat_press_timeout_timer;
    TimerHandle_t long_press_timer;

    struct _button *next;
} button_t;

static SemaphoreHandle_t buttons_lock = NULL;
static button_t *buttons = NULL;

static TimerHandle_t toggle_timer = NULL;

static void button_toggle_callback(button_t *button) {

    if (button->last_high == (button->config.active_level == BUTTON_ACTIVE_HIGH)) {
        // pressed
        button->callback(BUTTON_EVENT_DOWN, button->context);
        button->press_count++;
        xTimerStart(button->repeat_press_timeout_timer, 1);
        if (button->config.long_press_time && button->press_count == 1) {
            xTimerStart(button->long_press_timer, 1);
        }
    } else {
        // released
        button->callback(BUTTON_EVENT_UP, button->context);
        if (button->long_press_timer
                && xTimerIsTimerActive(button->long_press_timer)) {
            xTimerStop(button->long_press_timer, 1);
        }
        if (button->repeat_press_timeout_timer
                    && !xTimerIsTimerActive(button->repeat_press_timeout_timer)) {
            button->callback(BUTTON_EVENT_UP_HOLD, button->context);
        }
    }
}


static void button_long_press_timer_callback(TimerHandle_t timer) {
    button_t *button = (button_t*) pvTimerGetTimerID(timer);

    button->callback(BUTTON_EVENT_LONG_PRESS, button->context);
    button->press_count = 0;
}

static void button_repeat_press_timeout_timer_callback(TimerHandle_t timer) {
    button_t *button = (button_t*) pvTimerGetTimerID(timer);

    // if it's still being pressed, then it's not a momentary press, it's 
    //  a 'press down and hold'
    if (button->last_high == (button->config.active_level == BUTTON_ACTIVE_HIGH)) {
        button->callback(BUTTON_EVENT_DOWN_HOLD, button->context);
    } else {
        button->callback(button->press_count, button->context);
    }
    button->press_count = 0;
}

// Check state of each gpio_num for change of state
static void toggle_timer_callback(TimerHandle_t timer) {
    if (xSemaphoreTake(buttons_lock, 0) != pdTRUE)
        return;

    button_t *button = buttons;

    while (button) {
        if (gpio_get_level(button->gpio_num) == 1) {
            button->value = MIN(button->value + 1, MAX_TOGGLE_VALUE);
            if (button->value == MAX_TOGGLE_VALUE && !button->last_high) {
                button->last_high = true;
                button_toggle_callback(button);
            }
        } else {
            button->value = MAX(button->value - 1, 0);
            if (button->value == 0 && button->last_high) {
                button->last_high = false;
                button_toggle_callback(button);
            }
        }
        button = button->next;
    }

    xSemaphoreGive(buttons_lock);
}

static void button_free(button_t *button) {
    if (button->repeat_press_timeout_timer) {
        xTimerStop(button->repeat_press_timeout_timer, 1);
        xTimerDelete(button->repeat_press_timeout_timer, 1);
    }
   if (button->repeat_press_timeout_timer) {
        xTimerStop(button->repeat_press_timeout_timer, 1);
        xTimerDelete(button->repeat_press_timeout_timer, 1);
    }
    free(button);
}

static int buttons_init() {
    if (!buttons_lock) {
        buttons_lock = xSemaphoreCreateBinary();
        xSemaphoreGive(buttons_lock);
    }
    return 0;
}

int button_create(const uint8_t gpio_num,
                  button_config_t config,
                  button_callback_fn callback,
                  void* context)
{
    // initialise buttons and create timer to monitor GPIO status ('toggle')
    if (!buttons_lock) {
        buttons_init();

        toggle_timer = xTimerCreate(
            "Toggle timer", pdMS_TO_TICKS(10), pdTRUE, NULL, toggle_timer_callback
        );
        if (!toggle_timer) {
            return -1;          // cannot create primary gpio monitoring task
        }
    }

    xSemaphoreTake(buttons_lock, portMAX_DELAY);
    button_t *button = buttons;
    while (button && button->gpio_num != gpio_num)
        button = button->next;

    bool exists = button != NULL;
    xSemaphoreGive(buttons_lock);

    if (exists)
        return -2;              // button already assigned to gpio_num

    button = malloc(sizeof(button_t));
    memset(button, 0, sizeof(*button));
    button->gpio_num = gpio_num;
    button->config = config;
    button->callback = callback;
    button->context = context;
 

    button->repeat_press_timeout_timer = xTimerCreate(
        "Button Repeat Timeout Timer", pdMS_TO_TICKS(config.repeat_press_timeout),
        pdFALSE, button, button_repeat_press_timeout_timer_callback
    );
    if (!button->repeat_press_timeout_timer) {
        button_free(button);
        return -3;              // cannot create repeat press timeout timer
    }

    if (config.long_press_time) {
        button->long_press_timer = xTimerCreate(
            "Button Long Press Timer", pdMS_TO_TICKS(config.long_press_time),
            pdFALSE, button, button_long_press_timer_callback
        );
        if (!button->long_press_timer) {
            button_free(button);
            return -4;          // cannot create long press timer
        }
    }

    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL<<button->gpio_num);
    if (gpio_config(&io_conf) != ESP_OK) {
        button_free(button);
        return -5;              // issue with gpio config
    }

    button->last_high = gpio_get_level(button->gpio_num) == 1;

    xSemaphoreTake(buttons_lock, portMAX_DELAY);

    button->next = buttons;
    buttons = button;

    xSemaphoreGive(buttons_lock);

    if (!xTimerIsTimerActive(toggle_timer)) {
        xTimerStart(toggle_timer, 1);
    }

    return 0;
}

void button_delete(const uint8_t gpio_num) {
    if (!buttons_lock) {
        buttons_init();
    }

    xSemaphoreTake(buttons_lock, portMAX_DELAY);

    if (!buttons) {
        xSemaphoreGive(buttons_lock);
        return;
    }

    button_t *button = NULL;
    if (buttons->gpio_num == gpio_num) {
        button = buttons;
        buttons = buttons->next;
    } else {
        button_t *b = buttons;
        while (b->next) {
            if (b->next->gpio_num == gpio_num) {
                button = b->next;
                b->next = b->next->next;
                break;
            }
        }
    }

    if (button) {
        button_free(button);
    }

    xSemaphoreGive(buttons_lock);
}
