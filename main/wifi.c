#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"              // For EventGroupHandle_t

#include "esp_log.h"                            // For ESP_LOGI
#include "esp_event.h"                          // For esp_event_base_t
#include "esp_err.h"

#include "esp_wifi_types.h"
#include "esp_wifi.h"
//#include "esp_timer.h"

#include "wifi.h"


static const char *TAG = "mywifi";

#define MAXIMUM_RETRY               4
static int s_retry_num = 0;

//static esp_timer_handle_t s_wifi_reconnect_timer;
SemaphoreHandle_t g_wifi_mutex = NULL;

static TaskHandle_t retry_connect_task_handle;

void start_ap_prov();

/* Event source periodic timer related definitions */
ESP_EVENT_DEFINE_BASE(CUSTOM_WIFI_EVENT);

/* Helper function to check for Mutex before issuing a connect command */
static void _wifi_connect(void* arg) {
    if( xSemaphoreTake(g_wifi_mutex, 100/portTICK_PERIOD_MS) == pdTRUE) {
        esp_wifi_connect();
    }
    else {
        ESP_LOGI(TAG, "Scan or connect in progress");
    }
}

static void retry_connect_task(void * arg)
{
    for(;;) {
        ESP_LOGI(TAG, "Retry connection task");
        _wifi_connect(NULL);
        vTaskDelay(10000/portTICK_PERIOD_MS);
    }
}

/* Event handler for Wi-Fi Events */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                MAC2STR(event->mac), event->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                MAC2STR(event->mac), event->aid);
        } else if (event_id == WIFI_EVENT_STA_START) {
            // If no valid SSID was found in NVS, then this will fail
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            #ifdef CONFIG_IDF_TARGET_ESP8266
            // ESP8266 RTOS SDK will continually retry every 2 seconds. To override, 
            //  (and to keep consistent with ESP-IDF) call esp_wifi_disconnect()
            esp_wifi_disconnect();
            #endif

            // Connect failed/finished, give back Mutex.
            xSemaphoreGive(g_wifi_mutex);

            if (s_retry_num < MAXIMUM_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Retry esp_wifi_connect() attempt %d of %d", s_retry_num, MAXIMUM_RETRY);
            } else {
                wifi_mode_t wifi_mode;
                esp_wifi_get_mode(&wifi_mode);
                if (wifi_mode != WIFI_MODE_APSTA) {
                    ESP_LOGI(TAG, "Failed to connect to AP. Start softAP Provisioning");
                    // Currently not in softAP (APSTA) mode. Start it.
                    start_ap_prov();
                }
                // Keep trying to connect to existing AP at a slower rate
                if (retry_connect_task_handle == NULL) {
                    xTaskCreate(&retry_connect_task, "retry_connect", 1024, NULL, tskIDLE_PRIORITY, &retry_connect_task_handle);
                }
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR "\n", IP2STR(&event->ip_info.ip));
            s_retry_num = 0;

            // Got IP - do not need softAP anymore
            // ********* Perform this after a timer *********
            //ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

            // Connect successful/finished, give back Mutex.
            xSemaphoreGive(g_wifi_mutex);
            // Stop periodic connection retry attempts
            //esp_timer_stop(s_wifi_reconnect_timer);
            if (retry_connect_task_handle != NULL) {
                vTaskDelete(retry_connect_task_handle);
                retry_connect_task_handle = NULL;
            }
        }
    } 
  
    else if (event_base == CUSTOM_WIFI_EVENT) {
        if (event_id == START_WIFI_SCAN) {
            ESP_LOGI(TAG, "Wi-Fi Scan Started");
            ESP_LOGW(TAG, "HEAP %d",  heap_caps_get_free_size(MALLOC_CAP_8BIT));
        } else if (event_id == START_WIFI_SCAN) {
            ESP_LOGI(TAG, "Wi-Fi Connect (from httpd) Started");
            ESP_LOGW(TAG, "HEAP %d",  heap_caps_get_free_size(MALLOC_CAP_8BIT));
        }
    }


}


void start_ap_prov() {
    wifi_config_t wifi_ap_config = {
        .ap = {
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    uint8_t mac;
    esp_read_mac(&mac, 1);
    snprintf((char *)wifi_ap_config.ap.ssid, 11, "esp_%02x%02x%02x", (&mac)[3], (&mac)[4], (&mac)[5]);
     
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); //must be called before esp_wifi_set_config()
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_ap_config));
 
    ESP_LOGI(TAG, "Started softAP ssid:%s", wifi_ap_config.ap.ssid);

}


void wifi_init()
{
    g_wifi_mutex = xSemaphoreCreateMutex();

/*
    esp_timer_create_args_t timer_args = {
        .callback = &_wifi_connect,
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_wifi_reconnect_timer));
*/

    // Mandatory Wi-Fi initialisation code
    #ifdef CONFIG_IDF_TARGET_ESP32
        ESP_ERROR_CHECK(esp_netif_init());             // previously tcpip_adapter_init()
        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();
    #elif CONFIG_IDF_TARGET_ESP8266
        tcpip_adapter_init();
    #endif

    // esp_event_handler_register is being deprecated
    #ifdef CONFIG_IDF_TARGET_ESP32
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(CUSTOM_WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    #elif CONFIG_IDF_TARGET_ESP8266
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(CUSTOM_WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    #endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Check to see if it has previosuly been provisioned.
    bool provisioned = false;

    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_cfg) != ESP_OK) {
 
    } else if (strlen((const char*) wifi_cfg.sta.ssid)) {
        ESP_LOGI(TAG, "Found ssid %s",     (const char*) wifi_cfg.sta.ssid);
        ESP_LOGI(TAG, "Found password %s", (const char*) wifi_cfg.sta.password);
        provisioned = true;
    }

    if (!provisioned) {
        start_ap_prov();               
    } else  {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    }
    ESP_ERROR_CHECK(esp_wifi_start());

}
