#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"              // For EventGroupHandle_t

#include "nvs.h"
#include "nvs_flash.h"

#include "wifi.h"
#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp_netif.h"                          // Must be included before esp_wifi_default.h
#include "esp_wifi_default.h"                   // For esp_netif_create_default_wifi_sta
#endif

#include "httpd.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <driver/gpio.h>



#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_image_format.h"
static const char *TAG = "main";




void on_wifi_ready();




const int led_gpio = 2;
bool led_on = false;

void led_write(bool on) {
    gpio_set_level(led_gpio, on ? 1 : 0);
}

void led_init() {
    gpio_set_direction(led_gpio, GPIO_MODE_OUTPUT);
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
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111"
};

void on_wifi_ready() {
    homekit_server_init(&config);
}






void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_DEBUG);      
    esp_log_level_set("httpd", ESP_LOG_INFO); 
    esp_log_level_set("httpd_uri", ESP_LOG_INFO);    
    esp_log_level_set("httpd_txrx", ESP_LOG_INFO);     
    esp_log_level_set("httpd_sess", ESP_LOG_INFO);
    esp_log_level_set("httpd_parse", ESP_LOG_INFO);  
    esp_log_level_set("vfs", ESP_LOG_INFO);     
 
    // Initialize NVS.
    #ifdef CONFIG_IDF_TARGET_ESP32
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);
    #elif CONFIG_IDF_TARGET_ESP8266
      //  nvs_flash_init() is called in startup.c with assert == ESP_OK. So if you change 
      //  partition size, this will fail. Comment the assert out and then nvs_flash_erase()
    #endif



    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init();

    led_init();

    start_webserver();
 



    const esp_partition_t *next = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(next, &app_desc);
    ESP_LOGI(TAG, "Magic word 0x%8x, App version %s, Proj Name %s, Time %s",
             app_desc.magic_word, app_desc.version, app_desc.project_name, app_desc.time);

    ESP_LOGI(TAG, "esp_image_header_t %d esp_image_segment_header_t %d esp_app_desc_t %d", sizeof(esp_image_header_t), sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

    const esp_app_desc_t *app_desc1;
    app_desc1 = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Magic word 0x%8x, App version %s, Proj Name %s, Time %s",
             app_desc1->magic_word, app_desc1->version, app_desc1->project_name, app_desc1->time);


    esp_image_header_t image_header;
    esp_partition_read(next, 0, &image_header, sizeof(esp_image_header_t));
    ESP_LOGI(TAG, "Magic word 0x%2x, Entry 0x%4x, SPI size %d",
             image_header.magic, image_header.entry_addr - 0x40201000, image_header.spi_size);



}
