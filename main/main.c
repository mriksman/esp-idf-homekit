#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"              // For EventGroupHandle_t

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
#include "led_status.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <driver/gpio.h>
#ifdef CONFIG_IDF_TARGET_ESP8266
#include "mdns.h"                               // ESP8266 RTOS SDK mDNS needs legacy STATUS_EVENT to be sent to it
#endif
ESP_EVENT_DEFINE_BASE(HOMEKIT_EVENT);           // Convert esp-homekit events into esp event system      



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

static uint8_t button1_idx = 1;
static uint8_t button1_gpio = 0;
static uint8_t light1_gpio = 2;

//static uint8_t button2_idx1 = 2;




static led_status_t led_status;
static bool paired = false;


void led_init() {
//    gpio_set_direction(status_led_gpio, GPIO_MODE_OUTPUT);
    gpio_set_direction(light1_gpio, GPIO_MODE_OUTPUT);

//    gpio_set_level(status_led_gpio, 1);
//    gpio_set_level(light1_gpio, 1);
}


void led_identify(homekit_value_t _value) {
    ESP_LOGI(TAG, "LED identify");

    led_status_signal(led_status, &identify);
}


void lightbulb_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_bool) {
        ESP_LOGI(TAG, "Invalid value format: %d", value.format);
        return;
    }

    int light_gpio = *((uint8_t*) context);

    homekit_characteristic_t *service_name = homekit_service_characteristic_by_type(
                _ch->service, HOMEKIT_CHARACTERISTIC_NAME 
            );
 
    ESP_LOGI(TAG, "lightbulb_callback. name %s, lightbulb gpio %d", service_name->value.string_value, light_gpio);

    gpio_set_level(light_gpio, value.bool_value ? 0 : 1);
}


homekit_characteristic_t lightbulb1_service = HOMEKIT_CHARACTERISTIC_(
    ON, false, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
		lightbulb_callback, .context=&light1_gpio
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
            HOMEKIT_CHARACTERISTIC(IDENTIFY, led_identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "LED"),
            &lightbulb1_service,
            NULL
        }),
        NULL
    }),
    NULL
};


#ifdef CONFIG_IDF_TARGET_ESP8266
/* Legacy event loop still required for the old (ESP8266 RTOS SDK) mDNS event loop dispatch */
esp_err_t legacy_event_handler(void *ctx, system_event_t *event) {
    mdns_handle_system_event(ctx, event);
    return ESP_OK;
}
#endif

/* Event handler for Events */
static void status_event_handler(void* arg, esp_event_base_t event_base,
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
    }
}

void homekit_on_event(homekit_event_t event) {
    esp_event_post(HOMEKIT_EVENT, event, NULL, sizeof(NULL), 100/portTICK_PERIOD_MS);
}

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .on_event = homekit_on_event,
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

void button_callback(button_event_t event, void* context) {
    int button_idx = *((uint8_t*) context);

    switch (event) {
        case button_event_long_press:
            // STATELESS_PROGRAMMABLE_SWITCH supports single, double and long press events
            ESP_LOGI(TAG, "button %d long press event. start AP", button_idx);  
            //start_ap_prov();        

            xTaskCreate(&start_ap_task, "Start AP", 1536, NULL, tskIDLE_PRIORITY, NULL);


            break;

        case button_event_down:
            // Can start timers here to determine 'long press' (if required)
            ESP_LOGI(TAG, "button %d down", button_idx);
            ESP_LOGI(TAG, "ligtbulb_service %d ", lightbulb1_service.value.bool_value);
            
            ESP_LOGW(TAG, "HEAP %d",  heap_caps_get_free_size(MALLOC_CAP_8BIT));
 
            char buffer[400];
            vTaskList(buffer);
            ESP_LOGI(TAG, buffer);

            break;
        case button_event_up:
            ESP_LOGI(TAG, "button %d up", button_idx);
 
            break;
        default:
            ESP_LOGI(TAG, "button %d pressed %d times", button_idx, event);
            if (event == 1) {
                lightbulb1_service.value.bool_value = !lightbulb1_service.value.bool_value;
                homekit_characteristic_notify(&lightbulb1_service, lightbulb1_service.value);
            } 
            
    }
}



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

//    ESP_ERROR_CHECK(nvs_flash_erase());
//    homekit_server_reset();


    ESP_ERROR_CHECK(esp_event_loop_create_default());

   // esp_event_handler_register is being deprecated
    #ifdef CONFIG_IDF_TARGET_ESP32
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, status_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, status_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(HOMEKIT_EVENT, ESP_EVENT_ANY_ID, status_event_handler, NULL, NULL));
    #elif CONFIG_IDF_TARGET_ESP8266
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, status_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, status_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(HOMEKIT_EVENT, ESP_EVENT_ANY_ID, status_event_handler, NULL));
    #endif

    #if CONFIG_IDF_TARGET_ESP8266
        /* mDNS in ESP8266 RTOS SDK 3.3 still relies on legacy system event loop 
           to dispatch events to it using mdns_handle_system_event */
        esp_event_loop_init(legacy_event_handler, NULL);
    #endif


    wifi_init();

    create_accessory_name();
    homekit_server_init(&config);

    led_init();

    paired = homekit_is_paired();
    led_status = led_status_init(status_led_gpio, false);


    button_config_t button_config = BUTTON_CONFIG(
        button_active_low,
        .repeat_press_timeout = 500,
        .long_press_time = 10000,
    );

    button_create(button1_gpio, button_config, button_callback, &button1_idx);






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
