#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"              // For EventGroupHandle_t
#include "driver/gpio.h"
#include <sys/param.h>                          // MIN MAX

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

#include "lights.h"                             // common struct used for NVS read/write of lights config

#include "esp_log.h"
static const char *TAG = "main";

// Have set lwip sockets from 10 to 16 (maximum allowed)
//   5 for httpd (down from default of 7)
//   12 for HomeKit (up from 8)

static led_status_t led_status;
static bool paired = false;

static led_status_pattern_t ap_mode = LED_STATUS_PATTERN({1000, -1000});
static led_status_pattern_t not_paired = LED_STATUS_PATTERN({100, -100});
static led_status_pattern_t normal_mode = LED_STATUS_PATTERN({5, -9995});
static led_status_pattern_t identify = LED_STATUS_PATTERN({100, -100, 100, -350, 100, -100, 100, -350, 100, -100, 100, -350});

typedef struct {
    // from NVS
    lights_t config;            // lights.h shared with httpd

    // private
    bool light_gpio_inverted;
    bool led_gpio_inverted;

    uint8_t idx;
    TimerHandle_t dim_timer;
    int8_t dim_direction;
    uint8_t pwm_channel;
} light_service_t;

static light_service_t *lights;

static homekit_accessory_t *accessories[2];


void status_led_identify(homekit_value_t _value) {
    led_status_signal(led_status, &identify);
}

void lightbulb_on_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_bool) {
        ESP_LOGE(TAG, "Invalid value format: %d", value.format);
        return;
    }

    uint8_t light_idx = *((uint8_t*) context);

    if (lights[light_idx].config.is_dimmer) {
        homekit_characteristic_t *brightness_c = homekit_service_characteristic_by_type(
                    _ch->service, HOMEKIT_CHARACTERISTIC_BRIGHTNESS 
                );

        pwm_set_duty(lights[light_idx].pwm_channel, value.bool_value ? brightness_c->value.int_value * PWM_PERIOD_IN_US/100 : 0);
        pwm_start();

        // if the light is turned off, set the direction up for the next time the light turns on
        //  it makes sense that, if it turns on greater than 50% brightness, that the next
        //  dim direction should be to dim the lights.
        if (value.bool_value == false) {
            lights[light_idx].dim_direction = brightness_c->value.int_value > 50 ? -1 : 1;
        }
    }
    else {
        gpio_set_level(lights[light_idx].config.light_gpio, lights->light_gpio_inverted ? !value.bool_value : value.bool_value); 
    }
}

void lightbulb_brightness_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_int) {
        ESP_LOGE(TAG, "Invalid value format: %d", value.format);
        return;
    }
    uint8_t light_idx = *((uint8_t*) context);

    pwm_set_duty(lights[light_idx].pwm_channel, value.int_value * PWM_PERIOD_IN_US/100);
    pwm_start();

}

static void light_dim_timer_callback(TimerHandle_t timer) {
    uint8_t light_idx = *((uint8_t*) pvTimerGetTimerID(timer));

    // service[0] is the accessory name/manufacturer. 
    // following that are the lights in service[1], service[2], ...
    uint8_t service_idx = light_idx + 1; 

    // Get the service and characteristics
    homekit_accessory_t *accessory = accessories[0];
    homekit_service_t *service = accessory->services[service_idx];

    homekit_characteristic_t *on_c = service->characteristics[1];
    homekit_characteristic_t *brightness_c = service->characteristics[2];

    int brightness = brightness_c->value.int_value;
    brightness = MIN(100, brightness + 2*lights[light_idx].dim_direction);
    brightness = MAX(10, brightness);
    brightness_c->value = HOMEKIT_INT(brightness);
    homekit_characteristic_notify(brightness_c, HOMEKIT_INT(brightness));

    bool on = on_c->value.bool_value;
    on = brightness > 0 ? true : false;
    on_c->value = HOMEKIT_BOOL(on);
    homekit_characteristic_notify(on_c, HOMEKIT_BOOL(on));

    // don't continue running timer if max/min has been reached
    if ( brightness == 10 || brightness == 100) {
        xTimerStop(lights[light_idx].dim_timer, 0);
    }
}

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
        }
        else if (event_id == HOMEKIT_EVENT_CLIENT_DISCONNECTED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_CLIENT_DISCONNECTED");
        }
        else if (event_id == HOMEKIT_EVENT_PAIRING_ADDED || event_id == HOMEKIT_EVENT_PAIRING_REMOVED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_PAIRING_ADDED or HOMEKIT_EVENT_PAIRING_REMOVED");
            paired = homekit_is_paired();
            led_status_set(led_status, paired ? &normal_mode : &not_paired);
        }
    } else if (event_base == BUTTON_EVENT) {
        uint8_t light_idx = *((uint8_t*) event_data);

        if (event_id == BUTTON_EVENT_UP) {
            gpio_set_level(lights[light_idx].config.led_gpio, lights->led_gpio_inverted ? 1 : 0);
        }
        else if (event_id == BUTTON_EVENT_DOWN) {
            gpio_set_level(lights[light_idx].config.led_gpio, lights->led_gpio_inverted ? 0 : 1);
        }

        else if (event_id == BUTTON_EVENT_DOWN_HOLD) {
            if (lights[light_idx].config.is_dimmer) {
                xTimerStart(lights[light_idx].dim_timer, 0);
            }
        }
        else if (event_id == BUTTON_EVENT_UP_HOLD) {
            if (lights[light_idx].config.is_dimmer) {
                xTimerStop(lights[light_idx].dim_timer, 0);
                lights[light_idx].dim_direction *= -1;
            }
        }

        else if (event_id == BUTTON_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "button %d long press event. start AP", light_idx);  
            //start_ap_prov();        
            xTaskCreate(&start_ap_task, "Start AP", 1536, NULL, tskIDLE_PRIORITY, NULL);
        }
        else {
            // service[0] is the accessory name/manufacturer. 
            // following that are the lights in service[1], service[2], ...
            uint8_t service_idx = light_idx + 1; 

            // Get the service and characteristics
            homekit_accessory_t *accessory = accessories[0];
            homekit_service_t *service = accessory->services[service_idx];
            homekit_characteristic_t *on_c = service->characteristics[1];
            homekit_characteristic_t *brightness_c = service->characteristics[2];

            if (event_id == 1) {
                // Toggle ON
                bool on = on_c->value.bool_value;
                on_c->value = HOMEKIT_BOOL(!on);
                homekit_characteristic_notify(on_c, HOMEKIT_BOOL(!on));
            } 
            else if (event_id == 2) {
                if (lights[light_idx].config.is_dimmer) {
                    // On and full brightness
                    brightness_c->value = HOMEKIT_INT(100);
                    homekit_characteristic_notify(brightness_c, HOMEKIT_INT(100));
                    on_c->value = HOMEKIT_BOOL(true);
                    homekit_characteristic_notify(on_c, HOMEKIT_BOOL(true));

                    lights[light_idx].dim_direction = -1;
                }
            } 


            else if (event_id == 4) {
                ESP_LOGW(TAG, "HEAP %d",  heap_caps_get_free_size(MALLOC_CAP_8BIT));

                char buffer[400];
                vTaskList(buffer);
                ESP_LOGI(TAG, "\n%s", buffer);
            } 
        }
    }
}

void homekit_on_event(homekit_event_t event) {
    esp_event_post(HOMEKIT_EVENT, event, NULL, sizeof(NULL), 10);
}
void button_callback(button_event_t event, void* context) {
    // esp_event_post sends a pointer to a COPY of the data.
    esp_event_post(BUTTON_EVENT, event, context, sizeof(uint8_t), 10);
}


homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .on_event = homekit_on_event,
};

void init_accessory(uint8_t num_lights) {
    uint8_t macaddr[6];
    esp_read_mac(macaddr, ESP_MAC_WIFI_STA);
    int name_len = snprintf( NULL, 0, "esp_%02x%02x%02x", macaddr[3], macaddr[4], macaddr[5] );
    char *name_value = malloc(name_len + 1);
    snprintf( name_value, name_len + 1, "esp_%02x%02x%02x", macaddr[3], macaddr[4], macaddr[5] ); 

    homekit_service_t* services[num_lights + 1];
    homekit_service_t** s = services;

    *(s++) = NEW_HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
        NEW_HOMEKIT_CHARACTERISTIC(NAME, name_value),
        NEW_HOMEKIT_CHARACTERISTIC(MANUFACTURER, "HaPK"),
        NEW_HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "037A2BABF19D"),
        NEW_HOMEKIT_CHARACTERISTIC(MODEL, "MyLights"),
        NEW_HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1"),
        NEW_HOMEKIT_CHARACTERISTIC(IDENTIFY, status_led_identify),
        NULL
    });

    for (int i=0; i < num_lights; i++) {
        int light_name_len = snprintf(NULL, 0, "Light %d", i + 1);
        char *light_name_value = malloc(light_name_len + 1);
        snprintf(light_name_value, light_name_len + 1, "Light %d", i + 1);

        homekit_characteristic_t* characteristics[lights[i].config.is_dimmer ? 3 : 2]; // NAME, ON and if a dimmer BRIGHTNESS
        homekit_characteristic_t** c = characteristics;

        *(c++) = NEW_HOMEKIT_CHARACTERISTIC(NAME, light_name_value);
        *(c++) = NEW_HOMEKIT_CHARACTERISTIC(
                ON, false,
                .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
                    lightbulb_on_callback, .context=(void*)&lights[i].idx
                ),
            );
        if (lights[i].config.is_dimmer) {
            *(c++) = NEW_HOMEKIT_CHARACTERISTIC(
                    BRIGHTNESS, 100,
                    .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
                        lightbulb_brightness_callback, .context=(void*)&lights[i].idx
                    ),
                );
        }
        *(c++) = NULL;

        *(s++) = NEW_HOMEKIT_SERVICE(LIGHTBULB, .characteristics=characteristics);
    }
    *(s++) = NULL;

    accessories[0] = NEW_HOMEKIT_ACCESSORY(.category=homekit_accessory_category_lightbulb, .services=services);
    accessories[1] = NULL;
}


esp_err_t configure_peripherals(uint8_t num_lights) {
    esp_err_t err;
    size_t size;

    nvs_handle lights_config_handle;
    err = nvs_open("lights", NVS_READWRITE, &lights_config_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open err %d ", err);
        return err;
    }

    bool invert[4];
    size = 4;       //4; status_gpio, light_gpio, led_gpio, button_gpio
    err = nvs_get_blob(lights_config_handle, "invert", invert, &size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob invert err %d ", err);
        return err;
    }

    // Status LED
    uint8_t status_led_gpio = 0;
    err = nvs_get_u8(lights_config_handle, "status_led", &status_led_gpio); 
    if (err == ESP_OK) {
        led_status = led_status_init(status_led_gpio, invert[0] ? false : true);
    }
    else {
        ESP_LOGW(TAG, "error nvs_get_u8 status_led err %d", err);
    }

    // don't continue configuration unless there are lights to configure.
    if (num_lights == 0) {
        return ESP_FAIL;
    }

    lights = (light_service_t*)calloc(num_lights, sizeof(light_service_t));
    if (lights == NULL) {
        ESP_LOGE(TAG, "couldn't allocate mem for light_service_t");
        return ESP_FAIL; 
    }

    lights_t light_config[num_lights];
    size = num_lights * sizeof(lights_t);
    err = nvs_get_blob(lights_config_handle, "config", light_config, &size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob config err %d ", err);
        return err;
    }

    for (uint8_t i = 0; i < num_lights; i++) {
        lights[i].config.light_gpio = light_config[i].light_gpio; 
        lights[i].config.led_gpio = light_config[i].led_gpio;
        lights[i].config.button_gpio = light_config[i].button_gpio;
        lights[i].config.is_dimmer = light_config[i].is_dimmer; 
    }

    // used throughout main program
    lights->light_gpio_inverted = invert[1];
    lights->led_gpio_inverted = invert[2];

    // 1. button configuration
    button_config_t button_config = {
        .active_level = invert[3] ? BUTTON_ACTIVE_LOW : BUTTON_ACTIVE_HIGH,
        .repeat_press_timeout = 300,
        .long_press_time = 10000,
    };

    // 2. status/feedback LEDs on each button 
    //     OR 
    //    standard ON/OFF output for Light (not PWM/dimming)
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 0; 

    // 3. PWM configuration for dimming lights
    uint8_t num_dimming_lights = 0;
    uint8_t i_pwm = 0;
    for (uint8_t i = 0; i < num_lights; i++) {
        if (lights[i].config.is_dimmer) {
            num_dimming_lights++;
        }   
    }

    uint32_t pins[num_dimming_lights];
    uint32_t duties[num_dimming_lights];
    int16_t phases[num_dimming_lights];
    uint8_t pwm_invert_mask = 0;

    for (uint8_t i = 0; i < num_lights; i++) {
        lights[i].idx = i;

        // 1. button 
        err = button_create(lights[i].config.button_gpio, button_config, button_callback, &lights[i].idx);
        if (err != ESP_OK) {
            return err;
        }

        // 2. button feedback LED - add to bit mask ready for configuration
        io_conf.pin_bit_mask |= (1ULL<<lights[i].config.led_gpio);

        if (lights[i].config.is_dimmer) {
            // 3a. PWM configuration - add to pins array ready for configuration
            pins[i_pwm] = lights[i].config.light_gpio;
            duties[i_pwm] = 0;
            phases[i_pwm] = 0;
            pwm_invert_mask |= (lights->light_gpio_inverted << i_pwm);

            // create timer for long button hold - dimming function
            int dim_name_len = snprintf(NULL, 0, "dim%d", i + 1);
            char *dim_name_value = malloc(dim_name_len + 1);
            snprintf(dim_name_value, dim_name_len + 1, "dim%d", i + 1);
            // 100ms per 2% is 5s
            lights[i].dim_timer = xTimerCreate(
                dim_name_value, pdMS_TO_TICKS(100), pdTRUE, &lights[i].idx, light_dim_timer_callback
            );

            lights[i].dim_direction = -1;

            // store PWM channel number and incrememnt
            lights[i].pwm_channel = i_pwm;
            i_pwm++;
        }
        else {
            // 3b. Not PWM. GPIO will be standard OUTPUT driving a relay/lightswitch
            //      Add to list of GPIO that will be OUTPUT
            io_conf.pin_bit_mask |= (1ULL<<lights[i].config.light_gpio);
        }
    }

    // 2. configure button feedback LEDs and standard on/off lights (not dimming)
    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return err;
    }

    // turn off all status feedback LEDs (1 = off) and Lights that aren't PWM (1 = off)
    for (uint8_t i = 0; i < num_lights; i++) {
        gpio_set_level(lights[i].config.led_gpio, lights->led_gpio_inverted ? 1 : 0);
        if (!lights[i].config.is_dimmer) {
            gpio_set_level(lights[i].config.light_gpio, lights->light_gpio_inverted ? 1 : 0);
        }
    }

    // 3. configure PWM
    if (num_dimming_lights > 0) {
        pwm_init(PWM_PERIOD_IN_US, duties, num_dimming_lights, pins);    
        pwm_set_channel_invert(pwm_invert_mask);        // parameter is a bit mask
        pwm_set_phases(phases);                         // throws an error if not set (even if it's 0)
        pwm_start();
    }

    nvs_close(lights_config_handle);
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err;

    esp_log_level_set("*", ESP_LOG_DEBUG);      
    esp_log_level_set("httpd", ESP_LOG_INFO); 
    esp_log_level_set("httpd_uri", ESP_LOG_INFO);    
    esp_log_level_set("httpd_txrx", ESP_LOG_INFO);     
//    esp_log_level_set("httpd_sess", ESP_LOG_INFO);
    esp_log_level_set("httpd_parse", ESP_LOG_INFO);  
    esp_log_level_set("vfs", ESP_LOG_INFO);     
    esp_log_level_set("esp_timer", ESP_LOG_INFO);     
 
    // Initialize NVS. 
    // Note: esp82666 calls assert(nvs_flash_init()) in startup.c before app_main()
    // so this will have failed before reaching here. use 'idf.py erase_flash'
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {     // can happen if truncated/partition size changed
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

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

    wifi_init();


    nvs_handle lights_config_handle;
    err = nvs_open("lights", NVS_READWRITE, &lights_config_handle);
    if (err == ESP_OK) {
        // Get configured number of lights
        uint8_t num_lights = 0;
        err = nvs_get_u8(lights_config_handle, "num_lights", &num_lights);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "error nvs_get_u8 num_lights err %d", err);
        }
        nvs_close(lights_config_handle);

        // if num_lights == 0, it will configure status LED and then return an error
        if (configure_peripherals(num_lights) == ESP_OK) {
            init_accessory(num_lights);
            homekit_server_init(&config);
            paired = homekit_is_paired();
        }
    }
    else {
        ESP_LOGE(TAG, "nvs_open err %d ", err);
    }

}
