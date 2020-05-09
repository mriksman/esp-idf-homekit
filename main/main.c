#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"              // For EventGroupHandle_t
#include "driver/gpio.h"

#include "esp_event.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "wifi.h"
#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp_netif.h"                          // Must be included before esp_wifi_default.h
#include "esp_wifi_default.h"                   // For esp_netif_create_default_wifi_sta
#endif

#include "button.h"
ESP_EVENT_DEFINE_BASE(BUTTON_EVENT);            // Convert button events into esp event system      

#include "led_status.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#ifdef CONFIG_IDF_TARGET_ESP8266
#include "mdns.h"                               // ESP8266 RTOS SDK mDNS needs legacy STATUS_EVENT to be sent to it
#endif
ESP_EVENT_DEFINE_BASE(HOMEKIT_EVENT);           // Convert esp-homekit events into esp event system      

#include "pwm.h"
#define PWM_PERIOD_IN_US    1000                // in microseconds (1000us = 1kHz)

#include "esp_log.h"
static const char *TAG = "main";

// Have set lwip sockets from 10 to 16 (maximum allowed)
//   5 for httpd (down from default of 7)
//   12 for HomeKit (up from 8)

static led_status_t led_status;
static bool paired = false;

static led_status_pattern_t ap_mode = LED_STATUS_PATTERN({1000, -1000});
static led_status_pattern_t not_paired = LED_STATUS_PATTERN({100, -100});
static led_status_pattern_t pairing = LED_STATUS_PATTERN({100, -100, 100, -600});
static led_status_pattern_t normal_mode = LED_STATUS_PATTERN({5, -9995});
static led_status_pattern_t identify = LED_STATUS_PATTERN({100, -100, 100, -350, 100, -100, 100, -350, 100, -100, 100, -350});

#define MIN(a, b) (((b) < (a)) ? (b) : (a))
#define MAX(a, b) (((a) < (b)) ? (b) : (a))


// *** Included to show app_desc_t from partition ***
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
// **************************************************


homekit_characteristic_t lightbulb1_on;
homekit_characteristic_t lightbulb1_brightness;
homekit_characteristic_t lightbulb2_on;
homekit_characteristic_t lightbulb2_brightness;

// use variables instead of defines to pass these values around as context/args
static uint8_t status_led_gpio = 2;

typedef struct {
    uint8_t idx;
    uint8_t button_gpio;
    uint8_t light_gpio;
    uint8_t led_gpio;
    homekit_characteristic_t *lightbulb_on;
    homekit_characteristic_t *lightbulb_brightness;
    TimerHandle_t dim_timer;
    int8_t dim_direction;
} light_service_t;

static light_service_t lights[] = { 
    {
    .idx = 0,
    .button_gpio = GPIO_NUM_16,     //D0
    .light_gpio = GPIO_NUM_13,      //D7
    .led_gpio = GPIO_NUM_12,        //D6
    .lightbulb_on = &lightbulb1_on,
    .lightbulb_brightness = &lightbulb1_brightness,
    .dim_direction = -1,
    },
    {
    .idx = 1,
    .button_gpio = GPIO_NUM_5,      //D1 (will need to be GPIO 3 (RX) in final code)
    .light_gpio = GPIO_NUM_4,       //D2
    .led_gpio = GPIO_NUM_14,        //D5
    .lightbulb_on = &lightbulb2_on,
    .lightbulb_brightness = &lightbulb2_brightness,
    .dim_direction = -1,
    }
};


void status_led_identify(homekit_value_t _value) {
    led_status_signal(led_status, &identify);
}

void lightbulb_on_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_bool) {
        ESP_LOGI(TAG, "Invalid value format: %d", value.format);
        return;
    }

    homekit_characteristic_t *brightness = homekit_service_characteristic_by_type(
                _ch->service, HOMEKIT_CHARACTERISTIC_BRIGHTNESS 
            );
    light_service_t light = *((light_service_t*) context);

/*
    ESP_LOGI(TAG, "Characteristic ON; Light: %d, Bool_val: %d, Set PWM: %d", 
            light.idx,
            value.bool_value ? 1 : 0, 
            value.bool_value ? brightness->value.int_value * PWM_PERIOD_IN_US/100 : 0);
*/

    pwm_set_duty(light.idx, value.bool_value ? brightness->value.int_value * PWM_PERIOD_IN_US/100 : 0);
    pwm_start();

    // if the light is turned off, set the direction up for the next time the light turns on
    //  it makes sense that, if it turns on greater than 50% brightness, that the next
    //  dim direction should be to dim the lights.
    if (value.bool_value == false) {
        lights[light.idx].dim_direction = brightness->value.int_value > 50 ? -1 : 1;
    }

}
homekit_characteristic_t lightbulb1_on = HOMEKIT_CHARACTERISTIC_(
    ON, false, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_on_callback, .context=&lights[0]
	)
);
homekit_characteristic_t lightbulb2_on = HOMEKIT_CHARACTERISTIC_(
    ON, false, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_on_callback, .context=&lights[1]
	)
);

void lightbulb_brightness_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_int) {
        ESP_LOGI(TAG, "Invalid value format: %d", value.format);
        return;
    }
    light_service_t light = *((light_service_t*) context);
/*
    ESP_LOGI(TAG, "Characteristic BRIGHTNESS; Light: %d, Brightness_val: %d, Set PWM: %d", 
            light.idx,
            value.int_value, 
            value.int_value * PWM_PERIOD_IN_US/100);
*/
    pwm_set_duty(light.idx, value.int_value * PWM_PERIOD_IN_US/100);
    pwm_start();

}
homekit_characteristic_t lightbulb1_brightness = HOMEKIT_CHARACTERISTIC_(
    BRIGHTNESS, 100, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_brightness_callback, .context=&lights[0]
	)
);
homekit_characteristic_t lightbulb2_brightness = HOMEKIT_CHARACTERISTIC_(
    BRIGHTNESS, 100, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_brightness_callback, .context=&lights[1]
	)
);

static void light_dim_timer_callback(TimerHandle_t timer) {
    uint8_t light_idx = *((uint8_t*) pvTimerGetTimerID(timer));

    (lights[light_idx].lightbulb_brightness)->value.int_value = MAX(10, MIN( 100, (lights[light_idx].lightbulb_brightness)->value.int_value + lights[light_idx].dim_direction));
    homekit_characteristic_notify(lights[light_idx].lightbulb_brightness, (lights[light_idx].lightbulb_brightness)->value);

    (lights[light_idx].lightbulb_on)->value.bool_value = ((lights[light_idx].lightbulb_brightness)->value.int_value > 0) ? true : false;
    homekit_characteristic_notify(lights[light_idx].lightbulb_on, (lights[light_idx].lightbulb_on)->value);
}

homekit_characteristic_t name = HOMEKIT_CHARACTERISTIC_(NAME, "esp");

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_lightbulb, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            &name,
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "HaPK"),
            HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "037A2BABF19D"),
            HOMEKIT_CHARACTERISTIC(MODEL, "MyLights"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, status_led_identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Light 1"),
            &lightbulb1_on,
            &lightbulb1_brightness,
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Light 2"),
            &lightbulb2_on,
            &lightbulb2_brightness,
            NULL
        }),
        NULL
    }),
    NULL
};


// Need to call this function from a task different to the button_callback (executing in Tmr Svc)
// Have had occurrences when, if called from button_callback directly, the scheduler seems
// to lock up. 
static void start_ap_task(void * arg)
{
    ESP_LOGI(TAG, "Start AP task");
    start_ap_prov();
    vTaskDelete(NULL);
}

#ifdef CONFIG_IDF_TARGET_ESP8266
/* Legacy event loop still required for the old (ESP8266 RTOS SDK) mDNS event loop dispatch */
esp_err_t legacy_event_handler(void *ctx, system_event_t *event) {
    mdns_handle_system_event(ctx, event);
    return ESP_OK;
}
#endif

/* Event handler for Events */
static void main_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
            led_status_set(led_status, &ap_mode);
        } else if (event_id == WIFI_EVENT_AP_STOP) {
            led_status_set(led_status, paired ? &normal_mode : &not_paired);
        } 
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            wifi_mode_t wifi_mode;
            esp_wifi_get_mode(&wifi_mode);
            if (wifi_mode == WIFI_MODE_STA) {
                led_status_set(led_status, paired ? &normal_mode : &not_paired);
            } else {
                led_status_set(led_status, &ap_mode);
            }
        }
    } else if (event_base == HOMEKIT_EVENT) {
        if (event_id == HOMEKIT_EVENT_CLIENT_CONNECTED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_CLIENT_CONNECTED");
            if (!paired)
                led_status_set(led_status, &pairing);
        }
        else if (event_id == HOMEKIT_EVENT_CLIENT_DISCONNECTED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_CLIENT_DISCONNECTED");
            if (!paired)
                led_status_set(led_status, &not_paired);
        }
        else if (event_id == HOMEKIT_EVENT_PAIRING_ADDED || event_id == HOMEKIT_EVENT_PAIRING_REMOVED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_PAIRING_ADDED or HOMEKIT_EVENT_PAIRING_REMOVED");
            paired = homekit_is_paired();
            led_status_set(led_status, paired ? &normal_mode : &not_paired);
        }
    } else if (event_base == BUTTON_EVENT) {
        uint8_t light_idx = *((uint8_t*) event_data);

        if (event_id == BUTTON_EVENT_UP) {
            gpio_set_level(lights[light_idx].led_gpio, 1);
        }
        else if (event_id == BUTTON_EVENT_DOWN) {
            gpio_set_level(lights[light_idx].led_gpio, 0);
        }

        else if (event_id == BUTTON_EVENT_DOWN_HOLD) {
            xTimerStart(lights[light_idx].dim_timer, 1);
        }
        else if (event_id == BUTTON_EVENT_UP_HOLD) {
            xTimerStop(lights[light_idx].dim_timer, 1);
            lights[light_idx].dim_direction *= -1;
        }

        else if (event_id == BUTTON_EVENT_LONG_PRESS) {
            // STATELESS_PROGRAMMABLE_SWITCH supports single, double and long press events
            ESP_LOGI(TAG, "button %d long press event. start AP", light_idx);  
            //start_ap_prov();        
            xTaskCreate(&start_ap_task, "Start AP", 1536, NULL, tskIDLE_PRIORITY, NULL);
        }
        else {
            if (event_id == 1) {
                // Toggle ON
                (lights[light_idx].lightbulb_on)->value.bool_value = !(lights[light_idx].lightbulb_on)->value.bool_value;
                homekit_characteristic_notify(lights[light_idx].lightbulb_on, (lights[light_idx].lightbulb_on)->value);
            } 
            else if (event_id == 2) {
                // On and full brightness
                (lights[light_idx].lightbulb_brightness)->value.int_value = 100;
                homekit_characteristic_notify(lights[light_idx].lightbulb_brightness, (lights[light_idx].lightbulb_brightness)->value);
                (lights[light_idx].lightbulb_on)->value.bool_value = true;
                homekit_characteristic_notify(lights[light_idx].lightbulb_on, (lights[light_idx].lightbulb_on)->value);
            } 


            else if (event_id == 4) {
                ESP_LOGW(TAG, "HEAP %d",  heap_caps_get_free_size(MALLOC_CAP_8BIT));

                char buffer[400];
                vTaskList(buffer);
                ESP_LOGI(TAG, buffer);

                ESP_LOGW(TAG, "INTENABLE %x", xthal_get_intenable() );
            } 

            else if (event_id > 6 && event_id < 8) {
                esp_wifi_stop();
            } 
            else if (event_id > 10) {
                esp_wifi_start();
            }
        }
    }
}

void homekit_on_event(homekit_event_t event) {
    esp_event_post(HOMEKIT_EVENT, event, NULL, sizeof(NULL), 100/portTICK_PERIOD_MS);
}
void button_callback(button_event_t event, void* context) {
    // esp_event_post sends a pointer to a COPY of the data.
    esp_event_post(BUTTON_EVENT, event, context, sizeof(uint8_t), 100/portTICK_PERIOD_MS);
}

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .on_event = homekit_on_event,
};

void create_accessory_name() {
    uint8_t mac;
    esp_read_mac(&mac, 1);    
    int name_len = snprintf( NULL, 0, "esp_%02x%02x%02x", (&mac)[3], (&mac)[4], (&mac)[5] );
    char *name_value = malloc(name_len+1);
    snprintf( name_value, name_len+1, "esp_%02x%02x%02x", (&mac)[3], (&mac)[4], (&mac)[5] ); 
    name.value = HOMEKIT_STRING(name_value);
}

void configure_peripherals() {
    // Status/feedback LEDs on each button
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<lights[0].led_gpio) | (1ULL<<lights[1].led_gpio);
    gpio_config(&io_conf);

    gpio_set_level(lights[0].led_gpio, 1);      //1 is off
    gpio_set_level(lights[1].led_gpio, 1);

    // Status LED
    led_status = led_status_init(status_led_gpio, false);

    // Button configuration
    button_config_t button_config = BUTTON_CONFIG(
        BUTTON_ACTIVE_LOW,
        .repeat_press_timeout = 300,
        .long_press_time = 10000,
    );
    button_create(lights[0].button_gpio, button_config, button_callback, &lights[0].idx);
    button_create(lights[1].button_gpio, button_config, button_callback, &lights[1].idx);

    // PWM configuration for dimming lights
    uint32_t pins[] = { 
        lights[0].light_gpio,
        lights[1].light_gpio 
    };
    uint32_t duties[] = { 
        lightbulb1_on.value.bool_value ? lightbulb1_brightness.value.int_value * PWM_PERIOD_IN_US/100 : 0, 
        lightbulb2_on.value.bool_value ? lightbulb2_brightness.value.int_value * PWM_PERIOD_IN_US/100 : 0 
    };
    int16_t phases[] = { 
        0, 
        0 
    };
    pwm_init(PWM_PERIOD_IN_US, duties, sizeof(pins)/sizeof(pins[0]), pins);    
    pwm_set_channel_invert((1<<0) | (1<<1));        // parameter is a bit mask
    pwm_set_phases(phases);                         // throws an error if not set (even if it's 0)
    pwm_start();

    lights[0].dim_timer = xTimerCreate(
        "dim0", 5, pdTRUE, &lights[0].idx, light_dim_timer_callback
    );
    lights[1].dim_timer = xTimerCreate(
        "dim1", 5, pdTRUE, &lights[1].idx, light_dim_timer_callback
    );

}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_DEBUG);      
    esp_log_level_set("httpd", ESP_LOG_INFO); 
    esp_log_level_set("httpd_uri", ESP_LOG_INFO);    
    esp_log_level_set("httpd_txrx", ESP_LOG_INFO);     
//    esp_log_level_set("httpd_sess", ESP_LOG_INFO);
    esp_log_level_set("httpd_parse", ESP_LOG_INFO);  
    esp_log_level_set("vfs", ESP_LOG_INFO);     
    esp_log_level_set("esp_timer", ESP_LOG_INFO);     
 
    // Initialize NVS.
    #ifdef CONFIG_IDF_TARGET_ESP32
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);
    #elif CONFIG_IDF_TARGET_ESP8266
        // nvs_flash_init() is called in startup.c with assert == ESP_OK. So if you change 
        //  partition size, this will fail. Comment the assert out and then nvs_flash_erase()      
    #endif



    ESP_ERROR_CHECK(esp_event_loop_create_default());

   // esp_event_handler_register is being deprecated
    #ifdef CONFIG_IDF_TARGET_ESP32
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(HOMEKIT_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(BUTTON_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL, NULL));
    #elif CONFIG_IDF_TARGET_ESP8266
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(HOMEKIT_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(BUTTON_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
    #endif

    #if CONFIG_IDF_TARGET_ESP8266
        /* mDNS in ESP8266 RTOS SDK 3.3 still relies on legacy system event loop 
           to dispatch events to it using mdns_handle_system_event */
        esp_event_loop_init(legacy_event_handler, NULL);
    #endif


    configure_peripherals();

    wifi_init();

    create_accessory_name();
    homekit_server_init(&config);

    paired = homekit_is_paired();








    const esp_partition_t *next = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(next, &app_desc);
    const esp_app_desc_t *app_desc1;
    app_desc1 = esp_ota_get_app_description();

    ESP_LOGI(TAG, "\r\n\
                                Magic word   App version    Proj Name     Time  \r\n\
ota_get_partition_description   0x%8x  %s   %s   %s\r\n\
ota_get_app_description         0x%8x  %s   %s   %s",
             app_desc.magic_word, app_desc.version, app_desc.project_name, app_desc.time,
             app_desc1->magic_word, app_desc1->version, app_desc1->project_name, app_desc1->time
            );


}
