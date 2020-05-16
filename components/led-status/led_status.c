#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include <stdlib.h>
#include <stdbool.h>

#include <driver/gpio.h>

#include "led_status_private.h"

#define ABS(x) (((x) < 0) ? -(x) : (x))

typedef struct {
    uint8_t gpio;
    uint8_t active;
    TimerHandle_t timer;

    int n;
    led_status_pattern_t *pattern;              // led_status_set    -> repeating
    led_status_pattern_t *signal_pattern;       // led_status_signal -> 1 shot
} led_status_t;


static void led_status_write(led_status_t *status, bool on) {
    gpio_set_level(status->gpio, on ? status->active : !status->active);
}

static void led_status_tick_callback(TimerHandle_t timer);

static void led_status_tick(led_status_t *status) {
    led_status_pattern_t *p = status->signal_pattern ? status->signal_pattern : status->pattern;

    if (!p) {
//        sdk_os_timer_disarm(&status->timer);
        xTimerStop(status->timer, 1);

        led_status_write(status, false);
        return;
    }

    led_status_write(status, p->delay[status->n] > 0);

//    sdk_os_timer_arm(&status->timer, ABS(p->delay[status->n]), 0);
    xTimerChangePeriod(status->timer, pdMS_TO_TICKS(ABS(p->delay[status->n])), 1);
    xTimerStart(status->timer, 1);

    status->n = (status->n + 1) % p->n;
    if (status->signal_pattern && status->n == 0) {
        status->signal_pattern = NULL;
    }
}

static void led_status_tick_callback(TimerHandle_t timer) {
    led_status_t *status = (led_status_t*) pvTimerGetTimerID(timer);

    led_status_tick(status);
}

led_status_t *led_status_init(uint8_t gpio, uint8_t active_level) {
    led_status_t *status = malloc(sizeof(led_status_t));
    status->gpio = gpio;
    status->active = active_level;
    status->pattern = NULL;
    status->signal_pattern = NULL;
    status->timer = NULL;

    // set the timer, with an arbitrary time - but don't start
//    sdk_os_timer_setfn(&status->timer, (void(*)(void*))led_status_tick, status);
    status->timer = xTimerCreate(
        "Toggle timer", 100, pdFALSE, status, led_status_tick_callback
    );

    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<status->gpio);
    gpio_config(&io_conf);

    led_status_write(status, false);

    return status;
}

void led_status_done(led_status_t *status) {
//    sdk_os_timer_disarm(&status->timer);
    xTimerStop(status->timer, 1);
    xTimerDelete(status->timer, 1);

//    gpio_disable(status->gpio);

    free(status);
}

void led_status_set(led_status_t *status, led_status_pattern_t *pattern) {
    // if led_status_init has not been called and led_status_t pointer is NULL; just return
    if (status == NULL)
        return;

    status->pattern = pattern;

    if (!status->signal_pattern) {
        status->n = 0;
        led_status_tick(status);
    }
}

void led_status_signal(led_status_t *status, led_status_pattern_t *pattern) {
    if (status == NULL)
        return;

    if (!status->signal_pattern && !pattern)
        return;

    status->signal_pattern = pattern;
    status->n = 0;  // whether signal pattern is NULL or not, just reset the state
    led_status_tick(status);
}
