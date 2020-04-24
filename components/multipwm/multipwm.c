/*
 * Multi-channel PWM support for ESP8266 RTOS SDK using hw_timer (FRC1)
 * Based on multipwm for esp-open-rtos by Sashka Nochkin https://github.com/nochkin/multipwm
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "driver/hw_timer.h"

#include "esp_log.h"
static const char *TAG = "multipwm";

#include "multipwm.h"

#define hw_timer_intr_enable() _xt_isr_unmask(1 << ETS_FRC_TIMER1_INUM)
#define hw_timer_intr_disable() _xt_isr_mask(1 << ETS_FRC_TIMER1_INUM)



#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)  \
      (byte & 0x80 ? '1' : '0'), \
      (byte & 0x40 ? '1' : '0'), \
      (byte & 0x20 ? '1' : '0'), \
      (byte & 0x10 ? '1' : '0'), \
      (byte & 0x08 ? '1' : '0'), \
      (byte & 0x04 ? '1' : '0'), \
      (byte & 0x02 ? '1' : '0'), \
      (byte & 0x01 ? '1' : '0')

static void IRAM_ATTR multipwm_interrupt_handler(void *arg) {
    pwm_info_t *pwm_info = arg;
    pwm_schedule_t *curr = pwm_info->_schedule_current;
    pwm_schedule_t *curr_next = curr->next;
    uint32_t tick_next = curr_next->ticks;

    if (pwm_info->reverse) {
        GPIO_REG_WRITE(GPIO_OUT_W1TS_ADDRESS, curr->pins_c);
        GPIO_REG_WRITE(GPIO_OUT_W1TC_ADDRESS, curr->pins_s);
    } else {
        GPIO_REG_WRITE(GPIO_OUT_W1TS_ADDRESS, curr->pins_s);
        GPIO_REG_WRITE(GPIO_OUT_W1TC_ADDRESS, curr->pins_c);
    }

    if (tick_next == 0) {
        tick_next = pwm_info->_period;
    }

    pwm_info->_tick = curr->ticks;
    pwm_info->_schedule_current = curr_next;


//    timer_set_load(FRC1, tick_next - pwm_info->_tick);
    hw_timer_set_load_data(tick_next - pwm_info->_tick);
}

void multipwm_init(pwm_info_t *pwm_info) {
    pwm_info->_configured_pins = 0;

    pwm_info->_output = 0;
//    bzero(pwm_info->_schedule, MULTIPWM_MAX_CHANNELS * sizeof(pwm_schedule_t));
//    bzero(pwm_info->pins, MULTIPWM_MAX_CHANNELS * sizeof(pwm_pin_t));
    memset(pwm_info->_schedule, 0, MULTIPWM_MAX_CHANNELS * sizeof(pwm_schedule_t));
    memset(pwm_info->pins, 0, MULTIPWM_MAX_CHANNELS * sizeof(pwm_pin_t));


    pwm_info->_tick = 0;
    pwm_info->_period = MULTIPWM_MAX_PERIOD;

    pwm_info->_schedule[0].pins_s = 0;
    pwm_info->_schedule[0].pins_c = pwm_info->_configured_pins;
    pwm_info->_schedule[0].ticks = 0;
    pwm_info->_schedule[0].next = &pwm_info->_schedule[0];

    pwm_info->_schedule_current = &pwm_info->_schedule[0];

//    multipwm_stop(pwm_info);

//    _xt_isr_attach(INUM_TIMER_FRC1, multipwm_interrupt_handler, (_xt_isr)pwm_info);
//    hw_timer_intr_register(multipwm_interrupt_handler, (_xt_isr)pwm_info);
    hw_timer_init(multipwm_interrupt_handler, (_xt_isr)pwm_info);

    hw_timer_set_clkdiv(TIMER_CLKDIV_1);

    ESP_LOGI(TAG, "PWM Init");
}

void multipwm_set_pin(pwm_info_t *pwm_info, uint8_t channel, uint8_t pin) {
    if (channel >= pwm_info->channels) return;

    pwm_info->pins[channel].pin = pin;
    pwm_info->pins[channel].duty = 0;

//    gpio_enable(pin, GPIO_OUTPUT);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);

    pwm_info->_configured_pins |= 1 << pin;
}

void multipwm_dump_schedule(pwm_info_t *pwm_info) {
    ESP_LOGI(TAG, "Schedule:");
    pwm_schedule_t *pwm_schedule = &pwm_info->_schedule[0];
    do {
        ESP_LOGI(TAG, " - %5i: s: "BYTE_TO_BINARY_PATTERN" "BYTE_TO_BINARY_PATTERN" "BYTE_TO_BINARY_PATTERN"            c: "BYTE_TO_BINARY_PATTERN" "BYTE_TO_BINARY_PATTERN" "BYTE_TO_BINARY_PATTERN"",
                pwm_schedule->ticks,
                BYTE_TO_BINARY(pwm_schedule->pins_s >> 16),
                BYTE_TO_BINARY(pwm_schedule->pins_s >> 8),
                BYTE_TO_BINARY(pwm_schedule->pins_s),
                BYTE_TO_BINARY(pwm_schedule->pins_c >> 16),
                BYTE_TO_BINARY(pwm_schedule->pins_c >> 8),
                BYTE_TO_BINARY(pwm_schedule->pins_c)
                );
        pwm_schedule = pwm_schedule->next;
    } while (pwm_schedule->ticks > 0);
    ESP_LOGI(TAG, "clkdiv %d intr_type: %d reload: %d enable: %d load_data: %d count_data: %d",     
         hw_timer_get_clkdiv(),                             
         hw_timer_get_intr_type(),                          
         hw_timer_get_reload(),                             
         hw_timer_get_enable() ,
         hw_timer_get_load_data(),
         hw_timer_get_count_data()                            
         );


    
}

void multipwm_set_duty(pwm_info_t *pwm_info, uint8_t channel, uint16_t duty) {
    if (channel >= pwm_info->channels) return;

    pwm_info->pins[channel].duty = duty;
    uint8_t pin_n = pwm_info->pins[channel].pin;

    pwm_schedule_t *sched_prev = &pwm_info->_schedule[0];
    pwm_schedule_t *sched = sched_prev->next;

    if (duty == 0) {
        sched_prev->pins_s &= ~(1 << pin_n);
        sched_prev->pins_c |= (1 << pin_n);
    } else {
        sched_prev->pins_c &= ~(1 << pin_n);
        sched_prev->pins_s |= (1 << pin_n);
    }

    bool new_sched = false;
    pwm_schedule_t *new_sched_prev = NULL;
    pwm_schedule_t *new_sched_next = NULL;

    do {
        if (duty == sched->ticks) { // set when existing slice found
            sched->pins_s &= ~(1 << pin_n);
            sched->pins_c |= (1 << pin_n);
        } else {                    // remove from other slices
            if (sched->ticks > 0) {
                sched->pins_s &= ~(1 << pin_n);
                sched->pins_c &= ~(1 << pin_n);
            }
        }
        if ((duty > sched_prev->ticks) && (duty < sched->ticks)) {  // prepare to insert a new slice
            new_sched = true;
            new_sched_next = sched;
            new_sched_prev = sched_prev;
        } else if ((sched->next->ticks == 0) && (duty > sched->ticks)) {
            new_sched = true;
            new_sched_next = sched->next;
            new_sched_prev = sched;
        }
        sched_prev = sched;
        sched = sched->next;
    } while (sched->ticks > 0);

    if (new_sched) {
        for (uint8_t ii=1; ii<MULTIPWM_MAX_CHANNELS+1; ii++) {
            if (pwm_info->_schedule[ii].ticks == 0) {
                pwm_schedule_t *new_sched_pos = &pwm_info->_schedule[ii];
                new_sched_pos->pins_c |= (1 << pin_n);
                new_sched_pos->pins_s = 0;
                new_sched_pos->ticks = duty;
                new_sched_pos->next = new_sched_next;
                new_sched_prev->next = new_sched_pos;
                break;
            }
        }
    }

    // cleanup

//    bool running = timer_get_run(FRC1);   // return TIMER(frc).CTRL & BIT(7);
    bool running = hw_timer_get_enable();

    if (running) {
        multipwm_stop(pwm_info);
    }

    sched_prev = &pwm_info->_schedule[0];
    sched = sched_prev->next;
    do {
        if ((sched->pins_s == 0) && (sched->pins_c == 0)) {
            pwm_info->_schedule_current = &pwm_info->_schedule[0];
            pwm_info->_tick = 0;
            sched_prev->next = sched->next;
            sched->ticks = 0;
            break;
        }
        sched_prev = sched;
        sched = sched->next;
    } while (sched->ticks > 0);

    if (running) {
        multipwm_start(pwm_info);
    }

}

void multipwm_set_duty_all(pwm_info_t *pwm_info, uint16_t duty) {
    multipwm_stop(pwm_info);
    for (uint8_t ii=0; ii<pwm_info->channels; ii++) {
        multipwm_set_duty(pwm_info, ii, duty);
    }
    multipwm_start(pwm_info);
}

void multipwm_start(pwm_info_t *pwm_info) {

//    timer_set_load(FRC1, 1);
    hw_timer_set_load_data(1);

//    timer_set_reload(FRC1, false);        // sets frc1.ctrl.reload (bit 6) to false
    hw_timer_set_reload(true);

//    timer_set_interrupts(FRC1, true);     // this sets
    TM1_EDGE_INT_ENABLE();                  //  DPORT base register 0x3ff00000 + 0x04 
    hw_timer_intr_enable();                 //  and _xt_int_unmask

//    timer_set_run(FRC1, true);            // sets frc1.ctrl.en (bit 7) to true
    hw_timer_enable(true);

    ESP_LOGI(TAG, "PWM started");
}

void multipwm_stop(pwm_info_t *pwm_info) {

//    timer_set_interrupts(FRC1, false);
    hw_timer_intr_disable();
    TM1_EDGE_INT_DISABLE();

//    timer_set_run(FRC1, false);
    hw_timer_enable(false);

    ESP_LOGI(TAG, "PWM stopped");
}

