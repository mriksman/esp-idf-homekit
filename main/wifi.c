#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <freertos/timers.h>
//#include "freertos/event_groups.h"            // For EventGroupHandle_t

#include <string.h>

#include "esp_event.h"                          // For esp_event_base_t
#include "esp_err.h"

#include "esp_wifi_types.h"
#include "esp_wifi.h"
//#include "esp_timer.h"

#include "wifi.h"
#include "httpd.h"

#include "esp_log.h"                            // For ESP_LOGI
static const char *TAG = "mywifi";

static int s_retry_num = 0;

static TimerHandle_t g_retry_connect_timer = NULL;
static TimerHandle_t g_stop_delay_timer = NULL;
static SemaphoreHandle_t g_wifi_mutex = NULL;

void start_ap_prov();
void stop_ap_prov();

SemaphoreHandle_t* get_wifi_mutex() {
    return &g_wifi_mutex;
}

static void retry_connect_callback(TimerHandle_t timer) {
    ESP_LOGI(TAG, "Retry connection task");
    if( xSemaphoreTake(g_wifi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "Wi-Fi scan in progress");
    }
}

static void stop_delay_callback(TimerHandle_t timer) {
    stop_ap_prov();
}

/* Event handler for Wi-Fi Events */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            xTimerStop(g_stop_delay_timer, 0);
        }
        else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            esp_err_t err;
            wifi_sta_list_t connected_clients;
            err = esp_wifi_ap_get_sta_list(&connected_clients);
            ESP_LOGI(TAG, "num stations left %d, errno %d", connected_clients.num, err);

            #ifdef CONFIG_IDF_TARGET_ESP32
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                esp_netif_get_ip_info(netif, &ip_info);
                bool if_status = esp_netif_is_netif_up(netif);
            #elif CONFIG_IDF_TARGET_ESP8266
                tcpip_adapter_ip_info_t ip_info;
                ESP_ERROR_CHECK(tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info));
                bool if_status = tcpip_adapter_is_netif_up(TCPIP_ADAPTER_IF_STA);
            #endif

            // if the ESP is connected to an AP (router), and the last client disconnects, stop softAP
            if (if_status && err == ESP_OK && connected_clients.num == 0) {
                ESP_LOGI(TAG, "Last client disconnected from AP and ESP is connected to STA. Shutting down Soft AP");
                stop_ap_prov();
            }
        } 
        else if (event_id == WIFI_EVENT_STA_START) {
            // event generated with esp_wifi_start()
            esp_wifi_connect();
        } 
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {

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
                    esp_wifi_stop();        // stop/start - attempt to fix issue where it loses connection 
                    start_ap_prov();        // in STA mode to router, starts in APSTA mode, and cannot connect 
                    esp_wifi_start();       // to router (no AP found), and soft AP is not visible
                }
                // Keep trying to connect to existing AP at a slower rate
                xTimerStart(g_retry_connect_timer, 0);
            }
        }
    } 
    else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            s_retry_num = 0;

            // Connect successful/finished, give back Mutex.
            xSemaphoreGive(g_wifi_mutex);

            // Stop periodic connection retry attempts
            xTimerStop(g_retry_connect_timer, 0);

            wifi_mode_t wifi_mode;
            esp_wifi_get_mode(&wifi_mode);
            wifi_sta_list_t connected_clients;
            esp_wifi_ap_get_sta_list(&connected_clients);
            if (wifi_mode == WIFI_MODE_APSTA && connected_clients.num == 0) {
                ESP_LOGI(TAG, "Got an IP from STA, and no clients connected to Soft AP. Stopping Soft AP in %d seconds...", STOP_AP_DELAY/1000);
                xTimerStart(g_stop_delay_timer, 0);
                //stop_ap_prov();
            }
        }
    } 
}

void start_ap_prov() {
    wifi_config_t wifi_cfg;
    ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_AP, &wifi_cfg));

    if ( strncmp((const char*) wifi_cfg.ap.ssid, "esp-", 4) == 0 ) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); 
    } else {
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
        wifi_cfg.ap.max_connection = 4;

        uint8_t macaddr[6];
        esp_read_mac(macaddr, ESP_MAC_WIFI_SOFTAP);
        snprintf((char *)wifi_cfg.ap.ssid, 11, "esp-%02x%02x%02x", macaddr[3], macaddr[4], macaddr[5]);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));                        //must be called before esp_wifi_set_config()
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_cfg));

        ESP_LOGW(TAG, "No previous SSID found. Set to ssid %s  ", (const char*) wifi_cfg.ap.ssid);
    }

    start_webserver();
        
    ESP_LOGI(TAG, "Started softAP with SSID %s and started HTTPD Service", (const char*) wifi_cfg.ap.ssid);
}

void stop_ap_prov() {
    // Shutdown soft AP
    esp_wifi_set_mode(WIFI_MODE_STA);
    stop_webserver();

    ESP_LOGI(TAG, "Stopped softAP and HTTPD Service");
}

void wifi_init() {
    vSemaphoreCreateBinary(g_wifi_mutex);

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
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    #elif CONFIG_IDF_TARGET_ESP8266
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    #endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Always start softAP. If it connects to an AP, it stops after STOP_AP_DELAY.
    //  this allows the user to connect to the softAP after start-up in case a button wasn't
    //  configured (and thus, you can't access the configuration page)
    start_ap_prov();

    ESP_ERROR_CHECK(esp_wifi_start());

    g_retry_connect_timer = xTimerCreate(
        "retry_connect", pdMS_TO_TICKS(10000), pdTRUE, NULL, retry_connect_callback
    );
    g_stop_delay_timer = xTimerCreate(
        "delay_stop_ap", pdMS_TO_TICKS(STOP_AP_DELAY), pdFALSE, NULL, stop_delay_callback
    );

}
