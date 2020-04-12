#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"              // For EventGroupHandle_t

#include "esp_event_base.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "wifi.h"
#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp_netif.h"                          // Must be included before esp_wifi_default.h
#include "esp_wifi_default.h"                   // For esp_netif_create_default_wifi_sta
#endif

#include "httpd.h"
#include "button.h"
#include "led_status.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <driver/gpio.h>

// Have set lwip sockets from 10 to 16
//   5 for httpd (down from default of 7)
//   8 for HomeKit
//   3 for internal use

#define BUTTON1_GPIO 0
#define LED1_GPIO 2

static led_status_pattern_t unpaired = LED_STATUS_PATTERN({1000, -1000});
static led_status_pattern_t pairing = LED_STATUS_PATTERN({100, -100, 100, -600});
static led_status_pattern_t normal_mode = LED_STATUS_PATTERN({100, -9900});

void on_wifi_ready();


// *** Included to show app_desc_t from partition ***
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"

#include "esp_log.h"
static const char *TAG = "main";
// **************************************************




bool led_on = false;

void led_write(bool on) {
    gpio_set_level(LED1_GPIO, on ? 0 : 1);
}

void led_init() {
    gpio_set_direction(LED1_GPIO, GPIO_MODE_OUTPUT);
    led_write(led_on);
}

void led_identify_task(void *_args) {
    for (int i=0; i<3; i++) {
        for (int j=0; j<2; j++) {
            led_write(true);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            led_write(false);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        vTaskDelay(250 / portTICK_PERIOD_MS);
    }

    led_write(led_on);

    vTaskDelete(NULL);
}

void led_identify(homekit_value_t _value) {
    printf("LED identify\n");
    xTaskCreate(led_identify_task, "LED identify", 512, NULL, 2, NULL);
}

homekit_value_t led_on_get() {
    return HOMEKIT_BOOL(led_on);
}

void led_on_set(homekit_value_t value) {
    if (value.format != homekit_format_bool) {
        printf("Invalid value format: %d\n", value.format);
        return;
    }

    led_on = value.bool_value;
    led_write(led_on);
}

homekit_characteristic_t button_event = HOMEKIT_CHARACTERISTIC_(PROGRAMMABLE_SWITCH_EVENT, 0);

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_lightbulb, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Sample LED"),
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "HaPK"),
            HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "037A2BABF19D"),
            HOMEKIT_CHARACTERISTIC(MODEL, "MyLED"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, led_identify),
            NULL
        }),
        HOMEKIT_SERVICE(LIGHTBULB, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Sample LED"),
            HOMEKIT_CHARACTERISTIC(
                ON, false,
                .getter=led_on_get,
                .setter=led_on_set
            ),
            NULL
        }),
        HOMEKIT_SERVICE(STATELESS_PROGRAMMABLE_SWITCH, .characteristics=(homekit_characteristic_t*[]) {
            HOMEKIT_CHARACTERISTIC(NAME, "Button"),
            &button_event,
            NULL
        }),
        NULL
    }),
    NULL
};





static led_status_t led_status;
static bool paired = false;

void on_event(homekit_event_t event) {
    if (event == HOMEKIT_EVENT_SERVER_INITIALIZED) {
        led_status_set(led_status, paired ? &normal_mode : &unpaired);
 
        ESP_LOGI(TAG, "HOMEKIT_EVENT_SERVER_INITIALIZED paired %s", paired?"true":"false");

    }
    else if (event == HOMEKIT_EVENT_CLIENT_CONNECTED) {
        if (!paired)
            led_status_set(led_status, &pairing);

        ESP_LOGI(TAG, "HOMEKIT_EVENT_CLIENT_CONNECTED paired %s", paired?"true":"false");

    }
    else if (event == HOMEKIT_EVENT_CLIENT_DISCONNECTED) {
        if (!paired)
            led_status_set(led_status, &unpaired);

        ESP_LOGI(TAG, "HOMEKIT_EVENT_CLIENT_DISCONNECTED paired %s", paired?"true":"false");

    }
    else if (event == HOMEKIT_EVENT_PAIRING_ADDED || event == HOMEKIT_EVENT_PAIRING_REMOVED) {
        paired = homekit_is_paired();
        led_status_set(led_status, paired ? &normal_mode : &unpaired);

        ESP_LOGI(TAG, "HOMEKIT_EVENT_PAIRING_ADDED or HOMEKIT_EVENT_PAIRING_REMOVED paired %s", paired?"true":"false");
    }
}




homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .on_event = on_event,
};

void on_wifi_ready() {
    homekit_server_init(&config);
}


/* Event handler for Events */
static void homekit_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP");
            on_wifi_ready();
        }
    } 

}


// Multiple gang switches
uint8_t button_idx1 = 1;
uint8_t button_idx2 = 2;

void button_callback(button_event_t event, void* context) {
    int button_idx = *((uint8_t*) context);

    switch (event) {
        case button_event_down:
            // Can start timers here to determine 'long press' (if required)
            ESP_LOGI(TAG, "button %d down", button_idx);
                        homekit_characteristic_notify(&button_event, HOMEKIT_UINT8(0));
            break;
        case button_event_up:
            ESP_LOGI(TAG, "button %d up", button_idx);
                        homekit_characteristic_notify(&button_event, HOMEKIT_UINT8(1));
            break;
        default:
            ESP_LOGI(TAG, "button %d pressed %d times", button_idx, event);
                        homekit_characteristic_notify(&button_event, HOMEKIT_UINT8(2));
    }
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
//        
    #endif

//    ESP_ERROR_CHECK(nvs_flash_erase());
//    homekit_server_reset();


    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init();

    led_init();

    start_webserver();


    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &homekit_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &homekit_event_handler, NULL));
 

    button_config_t button_config = BUTTON_CONFIG(
        button_active_low,
    );

    button_create(BUTTON1_GPIO, button_config, button_callback, &button_idx1);


    paired = homekit_is_paired();
    led_status = led_status_init(LED1_GPIO, false);




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
