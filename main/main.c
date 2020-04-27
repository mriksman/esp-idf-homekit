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
ESP_EVENT_DEFINE_BASE(HOMEKIT_EVENT);            // Convert esp-homekit events into esp event system      


#include "multipwm.h"
#define PWM_PERIOD          MULTIPWM_MAX_PERIOD         //counts
#include "pwm.h"
#define PWM_PERIOD_IN_US    MULTIPWM_MAX_PERIOD/80      // in microseconds


// Have set lwip sockets from 10 to 16 (maximum allowed)
//   5 for httpd (down from default of 7)
//   12 for HomeKit (up from 8)



static led_status_pattern_t not_normal = LED_STATUS_PATTERN({1000, -1000});
static led_status_pattern_t pairing = LED_STATUS_PATTERN({100, -100, 100, -600});
static led_status_pattern_t normal_mode = LED_STATUS_PATTERN({100, -9900});
static led_status_pattern_t identify = LED_STATUS_PATTERN({100, -100, 100, -350, 100, -100, 100, -350, 100, -100, 100, -350});

void on_wifi_ready();


// *** Included to show app_desc_t from partition ***
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"

#include "esp_log.h"
static const char *TAG = "main";
// **************************************************







// use variables instead of defines to pass these values around as context/args
static uint8_t status_led_gpio = 2;

typedef struct {
    uint8_t idx;
    uint8_t button_gpio;
    uint8_t light_gpio;
} light_service_t;

static light_service_t light1 = {
    .idx = 1,
    .button_gpio = 0,
    .light_gpio = 4,
};


IRAM_ATTR pwm_info_t pwm_info;


//static uint8_t button2_idx1 = 2;


static led_status_t led_status;
static bool paired = false;


void status_led_init() {
    gpio_set_direction(status_led_gpio, GPIO_MODE_OUTPUT);
}

void status_led_identify(homekit_value_t _value) {
    ESP_LOGI(TAG, "LED identify");
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

    ESP_LOGI(TAG, "Characteristic ON; Bool_val: %d, Brightness_val: %d, Set PWM: %d", 
            value.bool_value ? 1 : 0, 
            brightness->value.int_value, 
            value.bool_value ? brightness->value.int_value * PWM_PERIOD_IN_US/100 : 0);


    light_service_t light = *((light_service_t*) context);

    pwm_set_duty(0, value.bool_value ? brightness->value.int_value * PWM_PERIOD_IN_US/100 : 0);
    pwm_start();

//    multipwm_set_duty(&pwm_info, 0, value.bool_value ? brightness->value.int_value * PWM_PERIOD/100 : 0);

}
homekit_characteristic_t lightbulb1_on = HOMEKIT_CHARACTERISTIC_(
    ON, false, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_on_callback, .context=&light1
	)
);

void lightbulb_brightness_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_int) {
        ESP_LOGI(TAG, "Invalid value format: %d", value.format);
        return;
    }

    ESP_LOGI(TAG, "Characteristic BRIGHTNESS; Brightness_val: %d, Set PWM: %d", 
            value.int_value, 
            value.int_value * PWM_PERIOD_IN_US/100);

   
    light_service_t light = *((light_service_t*) context);

    pwm_set_duty(0, value.int_value * PWM_PERIOD_IN_US/100);
    pwm_start();

//    multipwm_set_duty(&pwm_info, 0, value.int_value * PWM_PERIOD/100);
}
homekit_characteristic_t lightbulb1_brightness = HOMEKIT_CHARACTERISTIC_(
    BRIGHTNESS, 100, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_brightness_callback, .context=&light1
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




void resolve_mdns_host(const char * host_name)
{
    ESP_LOGI(TAG, "Query A: %s.local", host_name);

    struct ip4_addr addr;
    addr.addr = 0;

    esp_err_t err = mdns_query_a(host_name, 2000,  &addr);
    if(err){
        if(err == ESP_ERR_NOT_FOUND){
            printf("Host was not found!");
            return;
        }
        printf("Query Failed");
        return;
    }

    ESP_LOGI(TAG, IPSTR, IP2STR(&addr));
}





/* Event handler for Events */
static void main_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
            led_status_set(led_status, &not_normal);
        } else if (event_id == WIFI_EVENT_AP_STOP) {
            led_status_set(led_status, paired ? &normal_mode : &not_normal);
        } 
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            wifi_mode_t wifi_mode;
            esp_wifi_get_mode(&wifi_mode);
            if (wifi_mode == WIFI_MODE_STA) {
                led_status_set(led_status, paired ? &normal_mode : &not_normal);
            } else {
                led_status_set(led_status, &not_normal);
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
                led_status_set(led_status, &not_normal);
        }
        else if (event_id == HOMEKIT_EVENT_PAIRING_ADDED || event_id == HOMEKIT_EVENT_PAIRING_REMOVED) {
            ESP_LOGI(TAG, "HOMEKIT_EVENT_PAIRING_ADDED or HOMEKIT_EVENT_PAIRING_REMOVED");
            paired = homekit_is_paired();
            led_status_set(led_status, paired ? &normal_mode : &not_normal);
        }
    } else if (event_base == BUTTON_EVENT) {
        uint8_t button_idx = *((uint8_t*) event_data);

        if (event_id == BUTTON_EVENT_LONG_PRESS) {
            // STATELESS_PROGRAMMABLE_SWITCH supports single, double and long press events
            ESP_LOGI(TAG, "button %d long press event. start AP", button_idx);  
            //start_ap_prov();        
            xTaskCreate(&start_ap_task, "Start AP", 1536, NULL, tskIDLE_PRIORITY, NULL);
        }
        else if (event_id == BUTTON_EVENT_DOWN) {
            // Can start timers here to determine 'long press' (if required)
            ESP_LOGI(TAG, "button %d down", button_idx);
            ESP_LOGI(TAG, "ligtbulb_service %d ", lightbulb1_on.value.bool_value);
            
            ESP_LOGW(TAG, "HEAP %d",  heap_caps_get_free_size(MALLOC_CAP_8BIT));
 
            char buffer[400];
            vTaskList(buffer);
            ESP_LOGI(TAG, buffer);

            ESP_LOGW(TAG, "INTENABLE %x", xthal_get_intenable() );




            multipwm_dump_schedule(&pwm_info);



        }
        else if (event_id == BUTTON_EVENT_UP) {
            ESP_LOGI(TAG, "button %d up", button_idx);
        }
        else {
            ESP_LOGI(TAG, "button %d pressed %d times", button_idx, event_id);

            if (event_id == 1) {
                lightbulb1_on.value.bool_value = !lightbulb1_on.value.bool_value;
                homekit_characteristic_notify(&lightbulb1_on, lightbulb1_on.value);
            } 
            if (event_id == 2) {
//                _xt_isr_mask(1 << 10);
//                ESP_LOGW(TAG, "INTENABLE %x", xthal_get_intenable() );

    resolve_mdns_host("Mike-iPhone-11-Pro");

            } 

            if (event_id > 4 && event_id < 8) {
                esp_wifi_stop();
            } else if (event_id > 10) {
                esp_wifi_start();
            }

        }
    }
}

void homekit_on_event(homekit_event_t event) {
    esp_event_post(HOMEKIT_EVENT, event, NULL, sizeof(NULL), 100/portTICK_PERIOD_MS);
}
void button_callback(button_event_t event, void* context) {
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


    wifi_init();

    create_accessory_name();
    homekit_server_init(&config);

    status_led_init();

    paired = homekit_is_paired();
    led_status = led_status_init(status_led_gpio, false);


    button_config_t button_config = BUTTON_CONFIG(
        BUTTON_ACTIVE_LOW,
        .repeat_press_timeout = 500,
        .long_press_time = 10000,
    );

    button_create(light1.button_gpio, button_config, button_callback, &light1.idx);


    uint32_t pins[] = { light1.light_gpio };

    uint32_t duties[] = { lightbulb1_on.value.bool_value ? lightbulb1_brightness.value.int_value * PWM_PERIOD_IN_US/100 : 0 };

    pwm_init(PWM_PERIOD_IN_US, duties, 1, pins);    
    pwm_set_channel_invert(1);
    pwm_set_phase(0, 0);                // throws an error if not set
    pwm_start();

/*
    pwm_info.channels = 1;
    pwm_info.reverse = true;

    multipwm_init(&pwm_info);
    //multipwm_set_freq(&pwm_info, 65535);
    for (uint8_t i=0; i<pwm_info.channels; i++) {
        multipwm_set_pin(&pwm_info, i, pins[i]);
    }
    multipwm_set_duty(&pwm_info, 0, lightbulb1_on.value.bool_value ? lightbulb1_brightness.value.int_value * PWM_PERIOD/100 : 0);
    multipwm_start(&pwm_info);
*/

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
