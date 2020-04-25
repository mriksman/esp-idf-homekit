#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>

#include "mdns.h"                               // ESP8266 RTOS SDK mDNS needs legacy STATUS_EVENT to be sent to it

#include "multipwm.h"
#define PWM_PERIOD    (UINT16_MAX) //counts


#define SSID            "SSID"
#define AP_PASSWORD     "PASSWORD"


// Have set lwip sockets from 10 to 16 (maximum allowed)
//   5 for httpd (down from default of 7)
//   12 for HomeKit (up from 8)


#include "esp_log.h"
static const char *TAG = "main";



IRAM_ATTR pwm_info_t pwm_info;



void status_led_identify(homekit_value_t _value) {
    ESP_LOGI(TAG, "Identify");
}

void lightbulb_on_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_bool) {
        ESP_LOGI(TAG, "Invalid value format: %d", value.format);
        return;
    }

    homekit_characteristic_t *brightness = homekit_service_characteristic_by_type(
                _ch->service, HOMEKIT_CHARACTERISTIC_BRIGHTNESS 
            );

    ESP_LOGI(TAG, "Characteristic ON; Bool_val: %d, Brightness_val: %d, Set PWM: %d", 
            value.bool_value ? 1 : 0, 
            brightness->value.int_value, 
            value.bool_value ? brightness->value.int_value * PWM_PERIOD/100 : 0);

// using pwm.c library of ESP8266 RTOS SDK (uses WDEV TSF0)
/*
    pwm_set_duty(0, value.bool_value ? brightness->value.int_value * PWM_PERIOD/100 : 0);
    pwm_start();
*/

// using ported mulitpwm (uses FRC1)
    multipwm_set_duty(&pwm_info, 0, value.bool_value ? brightness->value.int_value * PWM_PERIOD/100 : 0);


}
homekit_characteristic_t lightbulb1_on = HOMEKIT_CHARACTERISTIC_(
    ON, false, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_on_callback
	)
);

void lightbulb_brightness_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_int) {
        ESP_LOGI(TAG, "Invalid value format: %d", value.format);
        return;
    }

    ESP_LOGI(TAG, "Characteristic BRIGHTNESS; Brightness_val: %d, Set PWM: %d", 
            value.int_value, 
            value.int_value * PWM_PERIOD/100);

// using pwm.c library of ESP8266 RTOS SDK (uses WDEV TSF0)
/*
    pwm_set_duty(0, value.int_value * PWM_PERIOD/100);
    pwm_start();
*/

// using ported mulitpwm (uses FRC1)
    multipwm_set_duty(&pwm_info, 0, value.int_value * PWM_PERIOD/100);

}
homekit_characteristic_t lightbulb1_brightness = HOMEKIT_CHARACTERISTIC_(
    BRIGHTNESS, 100, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_brightness_callback
	)
);

homekit_characteristic_t name = HOMEKIT_CHARACTERISTIC_(NAME, "esp");

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_lightbulb, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            &name,
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "HaPK"),
            HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "037A2BABF19D"),
            HOMEKIT_CHARACTERISTIC(MODEL, "MyLED"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, status_led_identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "LED"),
            &lightbulb1_on,
            &lightbulb1_brightness,
            NULL
        }),
        NULL
    }),
    NULL
};



esp_err_t legacy_event_handler(void *ctx, system_event_t *event) {
    mdns_handle_system_event(ctx, event);

    if (event->event_id == SYSTEM_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event->event_id == SYSTEM_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } 

    return ESP_OK;
}

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
};

void create_accessory_name() {
    uint8_t mac;
    esp_read_mac(&mac, 1);    
    int name_len = snprintf( NULL, 0, "esp_%02x%02x%02x", (&mac)[3], (&mac)[4], (&mac)[5] );
    char *name_value = malloc(name_len+1);
    snprintf( name_value, name_len+1, "esp_%02x%02x%02x", (&mac)[3], (&mac)[4], (&mac)[5] ); 
    name.value = HOMEKIT_STRING(name_value);
}

void app_main(void) {
    esp_log_level_set("esp_timer", ESP_LOG_INFO);   //mdns calls esp_timer_create which fires off a debug log every 100 ticks

    esp_event_loop_init(legacy_event_handler, NULL);

    create_accessory_name();
    homekit_server_init(&config);

    tcpip_adapter_init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = SSID,
            .password = AP_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());


/* 
// using pwm.c library of ESP8266 RTOS SDK (uses WDEV TSF0)
    uint32_t duties = 100;
    uint32_t pin_num = 2;               // LED is on GPIO 2

    pwm_init(PWM_PERIOD, duties, 1, pin_num);
    pwm_set_channel_invert(1);
    pwm_set_phases(phase);              // throws an error if not set
    pwm_start();
*/

// using ported mulitpwm (uses FRC1)
    uint8_t pins[] = { 2 };             // LED is on GPIO 2

    pwm_info.channels = 1;
    pwm_info.reverse = true;

    multipwm_init(&pwm_info);
    //multipwm_set_freq(&pwm_info, 65535);
    for (uint8_t i=0; i<pwm_info.channels; i++) {
        multipwm_set_pin(&pwm_info, i, pins[i]);
    }
    multipwm_start(&pwm_info);

}
