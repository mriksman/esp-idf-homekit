#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <freertos/timers.h>

#include "driver/gpio.h"
#include <sys/param.h>                          // min max functions
#include <math.h>                               // pow function
#include <string.h>

#include "pwm.h"
#define PWM_PERIOD_IN_US    1000                // in microseconds (1000us = 1kHz)

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "esp_wifi.h"
#include "wifi.h"
#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp_netif.h"                          // Must be included before esp_wifi_default.h
#include "esp_wifi_default.h"                   // For esp_netif_create_default_wifi_sta
#endif

#include "esp_http_client.h"
#include "cJSON.h"
#define MAX_HTTP_OUTPUT_BUFFER 2048

#include "button.h"
ESP_EVENT_DEFINE_BASE(BUTTON_EVENT);            // Convert button events into esp event system      
#include "led_status.h"

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
ESP_EVENT_DEFINE_BASE(HOMEKIT_EVENT);           // Convert esp-homekit events into esp event system      
#ifdef CONFIG_IDF_TARGET_ESP8266
#include "mdns.h"                               // ESP8266 RTOS SDK mDNS needs legacy STATUS_EVENT to be sent to it
#endif

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
static led_status_pattern_t remote_error = LED_STATUS_PATTERN({50, -50, 50, -50, 50, -50, 50, -50, 50, -50, 50, -50});
static led_status_pattern_t normal_mode = LED_STATUS_PATTERN({5, -9995});
static led_status_pattern_t identify = LED_STATUS_PATTERN({100, -100, 100, -350, 100, -100, 100, -350, 100, -100, 100, -350});

typedef struct _light {
    uint8_t light_gpio;
    uint8_t led_gpio;
    bool is_dimmer;
    bool is_remote;

    uint8_t hk_service_idx;

    TimerHandle_t dim_timer;
    int8_t dim_direction;
    uint8_t pwm_channel;            // only used with hardware PWM

    int remote_brightness;          // save during BRIGHTNESS_START
    cJSON *remote_resp;
    cJSON *nvs_command;
    char host_ip[20];               
    esp_http_client_handle_t client;

    struct _light *next;            // linked list
} light_service_t;

static light_service_t *lights = NULL;

// Invert status is common for each type (light, led, button, status) and 
//  is stored in NVS as a blob
bool light_gpio_inverted;
bool led_gpio_inverted;

static homekit_accessory_t *accessories[2];


void status_led_identify(homekit_value_t _value) {
    led_status_signal(led_status, &identify);
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (esp_http_client_is_chunked_response(evt->client)) {
                if (output_len + evt->data_len > MAX_HTTP_OUTPUT_BUFFER) {
                    ESP_LOGE(TAG, "cannot store more data - buffer full");
                }
                else if (evt->user_data) {
                    memcpy(evt->user_data + output_len, evt->data, evt->data_len);
                }
                output_len += evt->data_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            // null terminate string
            char *user_data = evt->user_data;
            user_data[output_len] = '\0';
            // reset length counter
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}


static QueueHandle_t q_remotehk_message_queue;

typedef enum {
    TOGGLE,
    FULL_ON,                    // this is the double-press event
    BRIGHTNESS_START,
    BRIGHTNESS_UPDATE,
    BRIGHTNESS_FINISHED,
} remote_hk_cmd_t;

typedef struct {
    light_service_t *light;
    remote_hk_cmd_t command;
} remote_hk_t;

static void remote_hk_task(void * arg)
{
    remote_hk_t hk_command;

    // address saved in esp_http_client_handle_t client handle
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

    while(1) {
        // receive a message from the queue to hold complete struct remote_hk_t structure.
        if (xQueueReceive(q_remotehk_message_queue, &(hk_command), portMAX_DELAY) == pdTRUE) {

            cJSON *root_cmd = NULL;

            char host_ip[20] = {0};
            char url_string[100] = {0};           

            if (hk_command.command == BRIGHTNESS_FINISHED) {
                ESP_LOGI(TAG, "BRIGHTNESS_FINISHED brightness %d direction %d", hk_command.light->remote_brightness, hk_command.light->dim_direction );
                hk_command.light->dim_direction *= -1;
                cJSON_Delete(hk_command.light->remote_resp);
                continue;
            }

            else if (hk_command.command == BRIGHTNESS_UPDATE) {
                ESP_LOGI(TAG, "BRIGHTNESS_UPDATE brightness %d direction %d", hk_command.light->remote_brightness, hk_command.light->dim_direction );
                // we saved the host and retrieved characteristics during BRIGHTNESS_START below
                strcpy(host_ip, hk_command.light->host_ip);
                cJSON *characteristics_json = cJSON_GetObjectItem(hk_command.light->remote_resp, "characteristics");

                // construct the command to send to the hk client as we go
                root_cmd = cJSON_CreateObject();
                cJSON *characteristics_cmd_json = cJSON_CreateArray();
                cJSON_AddItemToObject(root_cmd, "characteristics", characteristics_cmd_json);
    
                cJSON *characteristics_item;
                cJSON_ArrayForEach(characteristics_item, characteristics_json) {
                    cJSON *aid_key = cJSON_GetObjectItem(characteristics_item, "aid");
                    cJSON *iid_key  = cJSON_GetObjectItem(characteristics_item, "iid");
                    cJSON *type_key = cJSON_GetObjectItem(characteristics_item, "type");
                    cJSON *fld;

                    if (strcmp(type_key->valuestring, HOMEKIT_CHARACTERISTIC_BRIGHTNESS) == 0) {
                        cJSON_AddItemToArray(characteristics_cmd_json, fld = cJSON_CreateObject());
                        cJSON_AddItemToObject(fld, "aid", cJSON_CreateNumber(aid_key->valueint));
                        cJSON_AddItemToObject(fld, "iid", cJSON_CreateNumber(iid_key->valueint));
                        cJSON_AddItemToObject(fld, "value", cJSON_CreateNumber(hk_command.light->remote_brightness));
                    }
                    else if (strcmp(type_key->valuestring, HOMEKIT_CHARACTERISTIC_ON) == 0) {
                        cJSON_AddItemToArray(characteristics_cmd_json, fld = cJSON_CreateObject());
                        cJSON_AddItemToObject(fld, "aid", cJSON_CreateNumber(aid_key->valueint));
                        cJSON_AddItemToObject(fld, "iid", cJSON_CreateNumber(iid_key->valueint));
                        cJSON_AddItemToObject(fld, "value", cJSON_CreateBool(hk_command.light->remote_brightness > 0 ? true : false));
                    }
                }
            }

            // ******** TOGGLE, BRIGHTNESS_START, FULL_ON ********* //
            else {
                cJSON *host_json = cJSON_GetObjectItem(hk_command.light->nvs_command, "host");
                cJSON *payload_json = cJSON_GetObjectItem(hk_command.light->nvs_command, "payload");

    char *out = cJSON_PrintUnformatted(hk_command.light->nvs_command);
    ESP_LOGW("NVS PAYLOAD", "key: \"payload\" value: %s", out);
    free(out);

                // use 'host' and resolve IP address
                //    (bug esp-idf #5521) workaround: use mdns library to resolve hostname
                struct ip4_addr mdns_addr;
                mdns_addr.addr = 0;
                esp_err_t err = mdns_query_a(host_json->valuestring, 2000,  &mdns_addr);
                if(err) {
                    if(err == ESP_ERR_NOT_FOUND){
                        ESP_LOGE(TAG, "mdns_query_a error: %s was not found!", host_json->valuestring);
                    }
                    ESP_LOGE(TAG, "mdns_query_a error: query failed");
                    led_status_signal(led_status, &remote_error);
                    continue;
                }
                sprintf(host_ip, IPSTR, IP2STR(&mdns_addr));
                // end workaround **************************** //

    ESP_LOGW(TAG, "mdns address resolve %s", host_ip);

                // create aid.iid csv to send to remote hk device
                char aid_iid[20] = {0};
                cJSON *payload_item;
                cJSON_ArrayForEach(payload_item, payload_json) {
                    cJSON *key = cJSON_GetObjectItem(payload_item, "aid");
                    if (!cJSON_IsNumber(key)) { 
                        ESP_LOGE(TAG, "error parsing \"key\" : \"aid\"");
                        led_status_signal(led_status, &remote_error);
                        continue;
                    }
                    sprintf(aid_iid + strlen(aid_iid), "%d.", key->valueint);

                    key = cJSON_GetObjectItem(payload_item, "iid");
                    if (!cJSON_IsNumber(key)) { 
                        ESP_LOGE(TAG, "error parsing \"key\" : \"iid\"");
                        led_status_signal(led_status, &remote_error);
                        continue;
                    }
                    sprintf(aid_iid + strlen(aid_iid), "%d,", key->valueint);
                }
                // remove last ','
                aid_iid[strlen(aid_iid)-1] = '\0';

    ESP_LOGW(TAG, "aid_iid: %s", aid_iid);

                strcpy(url_string, "http://");
                strcat(url_string, host_ip);
                strcat(url_string, ":5556/characteristics?id=");
                strcat(url_string, aid_iid);
                strcat(url_string, "&type=1");

                if (!hk_command.light->client) {
                    //ESP8266 RTOS does not support .host and .port, only .url
                    esp_http_client_config_t config = {
                        .url = url_string,
                        .event_handler = _http_event_handler,
                        .user_data = local_response_buffer,
                    };
                    hk_command.light->client = esp_http_client_init(&config);
                    if (!hk_command.light->client) {
                        ESP_LOGE(TAG, "error esp_http_client_init");
                        led_status_signal(led_status, &remote_error);
                        continue;
                    }
                }

                esp_http_client_set_url(hk_command.light->client, url_string);
                esp_http_client_set_post_field(hk_command.light->client, NULL, 0);
                esp_http_client_set_method(hk_command.light->client, HTTP_METHOD_GET);
    
                // HTTP GET request to retrieve charactereistics of remote HK client
                // if the remote device has since restarted, the first try will fail
                err = esp_http_client_perform(hk_command.light->client);
                if (err != ESP_OK) {
                    esp_http_client_close(hk_command.light->client);

                    // retry
                    err = esp_http_client_perform(hk_command.light->client);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "GET request failed: 0x%x", err);
                        led_status_signal(led_status, &remote_error);
                        continue;
                    }
                }

    ESP_LOGW("RESPONSE", "%s", local_response_buffer);

                // **** parse the response returned from the hk client device ****//
                cJSON *root_resp = cJSON_Parse(local_response_buffer);
                cJSON *characteristics_json = cJSON_GetObjectItem(root_resp, "characteristics");
                if (!characteristics_json || !cJSON_IsArray(characteristics_json)) {
                    ESP_LOGE(TAG, "key: \"characteristics\" not found or is not a json array");
                    cJSON_Delete(root_resp);
                    led_status_signal(led_status, &remote_error);
                    continue;
                }

                if (hk_command.command == BRIGHTNESS_START) {
                    ESP_LOGI(TAG, "BRIGHTNESS_START brightness %d direction %d", hk_command.light->remote_brightness, hk_command.light->dim_direction );
                    // save all the information to be used in BRIGHTNESS_UPDATE commands
                    strcpy(hk_command.light->host_ip, host_ip);
                    // free cJSON when finished in BRIGHTNESS_FINISHED
                    hk_command.light->remote_resp = root_resp;
                    
                    cJSON *characteristics_item;
                    cJSON_ArrayForEach(characteristics_item, characteristics_json) {
                        cJSON *type_key = cJSON_GetObjectItem(characteristics_item, "type");
                        cJSON *value_key = cJSON_GetObjectItem(characteristics_item, "value");

                        if (strcmp(type_key->valuestring, HOMEKIT_CHARACTERISTIC_BRIGHTNESS) == 0) {
                            hk_command.light->remote_brightness = value_key->valueint;
                        }
                    }
                    // do not cJSON_Delete(root_resp) - will be cleaned up BRIGHTNESS_FINISHED
                    continue;
                }

                // construct the command to send to the hk client as we go
                root_cmd = cJSON_CreateObject();
                cJSON *characteristics_cmd_json = cJSON_CreateArray();
                cJSON_AddItemToObject(root_cmd, "characteristics", characteristics_cmd_json);

                cJSON *characteristics_item;
                cJSON_ArrayForEach(characteristics_item, characteristics_json) {
                    cJSON *aid_key = cJSON_GetObjectItem(characteristics_item, "aid");
                    cJSON *iid_key  = cJSON_GetObjectItem(characteristics_item, "iid");
                    cJSON *type_key = cJSON_GetObjectItem(characteristics_item, "type");
                    cJSON *value_key = cJSON_GetObjectItem(characteristics_item, "value");
                    cJSON *fld;

                    // OPTIONAL CHARACTERISTIC for remote switch identifier
                    if (strcmp(type_key->valuestring, "02B77067-DA5D-493C-829D-F6C5DCFE5C28") == 0){
                        // Go through the NVS payload and find what value to send to remote device;
                        cJSON_ArrayForEach(payload_item, payload_json) {
                            cJSON *nvs_aid = cJSON_GetObjectItem(payload_item, "aid");
                            cJSON *nvs_iid = cJSON_GetObjectItem(payload_item, "iid");
                            if (nvs_aid->valueint == aid_key->valueint &&
                                  nvs_iid->valueint == iid_key->valueint) {
                                cJSON *nvs_value = cJSON_GetObjectItem(payload_item, "value");
                                if (!cJSON_IsNumber(nvs_value)) { 
                                    ESP_LOGE(TAG, "error parsing \"key\" : \"nvs_value\"");
                                } else {
                                    cJSON_AddItemToArray(characteristics_cmd_json, fld = cJSON_CreateObject());
                                    cJSON_AddItemToObject(fld, "aid", cJSON_CreateNumber(aid_key->valueint));
                                    cJSON_AddItemToObject(fld, "iid", cJSON_CreateNumber(iid_key->valueint));
                                    cJSON_AddItemToObject(fld, "value", cJSON_CreateNumber(nvs_value->valueint));
                                }
                            }
                        }
                    }

                    else if (strcmp(type_key->valuestring, HOMEKIT_CHARACTERISTIC_BRIGHTNESS) == 0) {
                        if (hk_command.command == FULL_ON) {
                            cJSON_AddItemToArray(characteristics_cmd_json, fld = cJSON_CreateObject());
                            cJSON_AddItemToObject(fld, "aid", cJSON_CreateNumber(aid_key->valueint));
                            cJSON_AddItemToObject(fld, "iid", cJSON_CreateNumber(iid_key->valueint));
                            cJSON_AddItemToObject(fld, "value", cJSON_CreateNumber(100));
                        }
                    }

                    else if (strcmp(type_key->valuestring, HOMEKIT_CHARACTERISTIC_ON) == 0) {
                        cJSON_AddItemToArray(characteristics_cmd_json, fld = cJSON_CreateObject());
                        cJSON_AddItemToObject(fld, "aid", cJSON_CreateNumber(aid_key->valueint));
                        cJSON_AddItemToObject(fld, "iid", cJSON_CreateNumber(iid_key->valueint));
                        if (hk_command.command == FULL_ON) {
                            cJSON_AddItemToObject(fld, "value", cJSON_CreateBool(true));
                        } 
                        else if (hk_command.command == TOGGLE) {
                            cJSON_AddItemToObject(fld, "value", cJSON_CreateBool(!cJSON_IsTrue(value_key)));
                        }
                    }
                }
                // finished with 'root_resp' | 'characteristic_json'
                cJSON_Delete(root_resp);
            }
                       
            char *out = cJSON_PrintUnformatted(root_cmd);
            cJSON_Delete(root_cmd);

    ESP_LOGW("COMMAND", "key: \"characteristics\" value: %s", out);
            
            local_response_buffer[0] = '\0';

            strcpy(url_string, "http://");
            strcat(url_string, host_ip);
            strcat(url_string, ":5556/characteristics");

            esp_http_client_set_url(hk_command.light->client, url_string);
            esp_http_client_set_method(hk_command.light->client, HTTP_METHOD_PUT);
            esp_http_client_set_post_field(hk_command.light->client, out, strlen(out));
            esp_err_t err = esp_http_client_perform(hk_command.light->client);

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "HTTP POST Status = %d", esp_http_client_get_status_code(hk_command.light->client));

    ESP_LOGW("HTTP POST FINISHED", "%s", local_response_buffer);

            } else {
                ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
                led_status_signal(led_status, &remote_error);
            }    
            free(out);
        }
    }
}



void lightbulb_on_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_bool) {
        ESP_LOGE(TAG, "Invalid value format: %d", value.format);
        return;
    }

    light_service_t *light = (light_service_t*) context;

    if (light->is_dimmer) {
        homekit_characteristic_t *brightness_c = homekit_service_characteristic_by_type(
                    _ch->service, HOMEKIT_CHARACTERISTIC_BRIGHTNESS );
        double gamma = pow(((brightness_c->value.int_value+25.0)/125.0),2.2)*100.0;
        pwm_set_duty(light->pwm_channel, value.bool_value ? (uint32_t)(gamma * PWM_PERIOD_IN_US/100) : 0);
        pwm_start();

        // if the light is turned off, set the direction up for the next time the light turns on
        //  it makes sense that, if it turns on greater than 50% brightness, that the next
        //  dim direction should be to dim the lights.
        if (value.bool_value == false) {
            light->dim_direction = brightness_c->value.int_value > 50 ? -1 : 1;
        }
    }
    else {
        gpio_set_level(light->light_gpio, light_gpio_inverted ? !value.bool_value : value.bool_value); 
    }
}

void lightbulb_brightness_callback(homekit_characteristic_t *_ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_int) {
        ESP_LOGE(TAG, "Invalid value format: %d", value.format);
        return;
    }
    light_service_t *light = (light_service_t*) context;

    double gamma = pow(((value.int_value+25.0)/125.0),2.2)*100.0;
    pwm_set_duty(light->pwm_channel, (uint32_t)(gamma * PWM_PERIOD_IN_US/100));
    pwm_start();

}

static void light_dim_timer_callback(TimerHandle_t timer) {
    light_service_t *light = (light_service_t*) pvTimerGetTimerID(timer);

    int brightness = 0;

    if (light->is_remote) {
        // remote_brightness is first updated/cached during BRIGHTNESS_START call
        light->remote_brightness = MIN(100, light->remote_brightness + 5*light->dim_direction);
        light->remote_brightness = MAX(10, light->remote_brightness);
        brightness = light->remote_brightness;

        remote_hk_t remote_cmd = {
            .light = light,
            .command = BRIGHTNESS_UPDATE
        };
        // sizeof(struct remote_hk_t) bytes are copied from here into the queue
        xQueueSendToBack(q_remotehk_message_queue, (void *) &remote_cmd, (TickType_t) 0);
    }
    else {     
        // Get the service and characteristics
        homekit_accessory_t *accessory = accessories[0];
        homekit_service_t *service = accessory->services[light->hk_service_idx];
        homekit_characteristic_t *on_c = service->characteristics[1];
        homekit_characteristic_t *brightness_c = service->characteristics[2];

        brightness = brightness_c->value.int_value;
        brightness = MIN(100, brightness + 2*light->dim_direction);
        brightness = MAX(10, brightness);

        brightness_c->value = HOMEKIT_INT(brightness);
        homekit_characteristic_notify(brightness_c, HOMEKIT_INT(brightness));

        bool on = brightness > 0 ? true : false;
        on_c->value = HOMEKIT_BOOL(on);
        homekit_characteristic_notify(on_c, HOMEKIT_BOOL(on));
    }

    // don't continue if max/min has been reached
    if ( brightness == 10 || brightness == 100) {
        xTimerStop(light->dim_timer, 0);
    }

}

// Need to call this function from a task different to the button_callback (executing in Tmr Svc)
// Have had occurrences when, if called from button_callback directly, the scheduler seems
// to lock up. 
static void start_ap_task(void * arg) {
    ESP_LOGI(TAG, "start ap task");
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
        // convert the address into a pointer
        light_service_t *light = (light_service_t *) *((uintptr_t*)event_data);

        if (event_id == BUTTON_EVENT_UP) {
            gpio_set_level(light->led_gpio, led_gpio_inverted ? 1 : 0);
        }
        else if (event_id == BUTTON_EVENT_DOWN) {
            gpio_set_level(light->led_gpio, led_gpio_inverted ? 0 : 1);
        }

        else if (event_id == BUTTON_EVENT_DOWN_HOLD) {
            if (light->is_dimmer) {
                xTimerStart(light->dim_timer, 0);

                // grab the current brightness from the remote device
                if (light->is_remote) {
                    remote_hk_t remote_cmd = {
                        .light = light,
                        .command = BRIGHTNESS_START
                    };
                    // sizeof(struct remote_hk_t) bytes are copied from here into the queue
                    xQueueSendToBack(q_remotehk_message_queue, (void *) &remote_cmd, (TickType_t) 0);
                }
            }
        }
        else if (event_id == BUTTON_EVENT_UP_HOLD) {
            if (light->is_dimmer) {
                xTimerStop(light->dim_timer, 0);

                // clean up and set direction
                if (light->is_remote) {
                    remote_hk_t remote_cmd = {
                        .light = light,
                        .command = BRIGHTNESS_FINISHED
                    };
                    // sizeof(struct remote_hk_t) bytes are copied from here into the queue
                    xQueueSendToBack(q_remotehk_message_queue, (void *) &remote_cmd, (TickType_t) 0);
                }
                else {
                    light->dim_direction *= -1;
                }
            }
        }
        // start AP and web GUI for config
        else if (event_id == BUTTON_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "button long press event. start soft ap");  
            //start_ap_prov();        
            xTaskCreate(&start_ap_task, "start_ap", 1536, NULL, tskIDLE_PRIORITY, NULL);
        }
        else {
            // on/off
            if (event_id == 1) {
                if (light->is_remote) {
                    remote_hk_t remote_cmd = {
                        .light = light,
                        .command = TOGGLE
                    };
                    // sizeof(struct remote_hk_t) bytes are copied from here into the queue
                    xQueueSendToBack(q_remotehk_message_queue, (void *) &remote_cmd, (TickType_t) 0);
                }
                else {     
                    // Get the service and characteristics
                    homekit_accessory_t *accessory = accessories[0];
                    homekit_service_t *service = accessory->services[light->hk_service_idx];
                    homekit_characteristic_t *on_c = service->characteristics[1];
                    //homekit_characteristic_t *brightness_c = service->characteristics[2];
                    
                    // Toggle ON
                    bool on = on_c->value.bool_value;
                    on_c->value = HOMEKIT_BOOL(!on);
                    homekit_characteristic_notify(on_c, HOMEKIT_BOOL(!on));
                }
            } 
            // turn on full brightness
            else if (event_id == 2 && light->is_dimmer) {
                if (light->is_remote) {
                    remote_hk_t remote_cmd = {
                        .light = light,
                        .command = FULL_ON
                    };
                    // sizeof(struct remote_hk_t) bytes are copied from here into the queue
                    xQueueSendToBack(q_remotehk_message_queue, (void *) &remote_cmd, (TickType_t) 0);
                }
                else {     
                    // Get the service and characteristics
                    homekit_accessory_t *accessory = accessories[0];
                    homekit_service_t *service = accessory->services[light->hk_service_idx];
                    homekit_characteristic_t *on_c = service->characteristics[1];
                    homekit_characteristic_t *brightness_c = service->characteristics[2];
                    
                    // On and full brightness
                    brightness_c->value = HOMEKIT_INT(100);
                    homekit_characteristic_notify(brightness_c, HOMEKIT_INT(100));
                    on_c->value = HOMEKIT_BOOL(true);
                    homekit_characteristic_notify(on_c, HOMEKIT_BOOL(true));

                    light->dim_direction = -1;
                }
            } 
            // restart 
            else if (event_id == 5) {
                // use 'not_paired' flashing (fast flash) to indicate about to restart
                led_status_set(led_status, &not_paired);
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            } 
            // print heap usage to console
            else if (event_id == 10) {
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
    // esp_event_post sends a pointer to a COPY of the data. send address as uintptr_t data.
    uintptr_t light_addr = (uintptr_t)context;
    esp_event_post(BUTTON_EVENT, event, &light_addr, sizeof(uintptr_t), 10);
}


homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111",
    .on_event = homekit_on_event,
};

void init_accessory() {
    uint8_t macaddr[6];
    esp_read_mac(macaddr, ESP_MAC_WIFI_STA);
    int name_len = snprintf( NULL, 0, "esp-%02x%02x%02x", macaddr[3], macaddr[4], macaddr[5] );
    char *name_value = malloc(name_len + 1);
    snprintf( name_value, name_len + 1, "esp-%02x%02x%02x", macaddr[3], macaddr[4], macaddr[5] ); 

    homekit_service_t* services[5];    // maximum 4 lights + 1 for ACCESSORY_INFORMATION
    homekit_service_t** s = services;

    *(s++) = NEW_HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
        NEW_HOMEKIT_CHARACTERISTIC(NAME, name_value),
        NEW_HOMEKIT_CHARACTERISTIC(MANUFACTURER, "MikeKit"),
        NEW_HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "037A2BABF19D"),
        NEW_HOMEKIT_CHARACTERISTIC(MODEL, "MyLights"),
        NEW_HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "1.1"),
        NEW_HOMEKIT_CHARACTERISTIC(IDENTIFY, status_led_identify),
        NULL
    });

    light_service_t *light = lights;
    // service[0] is ACCESSORY_INFORMATION. Then we start the LIGHTBULB services at services[1]
    uint8_t hk_service_idx = 1;
    while (light) { 
        if (!light->is_remote) {
            int light_name_len = snprintf(NULL, 0, "Light %d", hk_service_idx);
            char *light_name_value = malloc(light_name_len + 1);
            snprintf(light_name_value, light_name_len + 1, "Light %d", hk_service_idx);

            homekit_characteristic_t* characteristics[light->is_dimmer ? 3 : 2]; // NAME, ON and if a dimmer BRIGHTNESS
            homekit_characteristic_t** c = characteristics;

            *(c++) = NEW_HOMEKIT_CHARACTERISTIC(NAME, light_name_value);
            *(c++) = NEW_HOMEKIT_CHARACTERISTIC(
                    ON, false,
                    .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
                        lightbulb_on_callback, .context=(void*)light
                    ),
                );
            if (light->is_dimmer) {
                *(c++) = NEW_HOMEKIT_CHARACTERISTIC(
                        BRIGHTNESS, 100,
                        .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(
                            lightbulb_brightness_callback, .context=(void*)light
                        ),
                    );
            }
            *(c++) = NULL;

            *(s++) = NEW_HOMEKIT_SERVICE(LIGHTBULB, .characteristics=characteristics);

            // save service array position to light config
            light->hk_service_idx = hk_service_idx;
            hk_service_idx++;
        }
        light = light->next;
    }
    *(s++) = NULL;

    accessories[0] = NEW_HOMEKIT_ACCESSORY(.category=homekit_accessory_category_lightbulb, .services=services);
    accessories[1] = NULL;
}

/* 
   Returns number of hardware lights attached to the switch.
    -1 error during configuration
     0 no hardware lights, only remote devices. do not start homekit!
    >0 at least one hardware device. start homekit.
*/
int8_t configure_peripherals() {
    nvs_handle lights_config_handle;
    esp_err_t err = nvs_open("lights", NVS_READWRITE, &lights_config_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open err %d ", err);
        return -1;
    }

    // Invert configuration for GPIOs; status_gpio [0], light_gpio [1], led_gpio [2], button_gpio [3]
    bool invert[4];
    size_t size = sizeof(invert)/sizeof(invert[0]);     
    err = nvs_get_blob(lights_config_handle, "invert", invert, &size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob invert err %d ", err);
        nvs_close(lights_config_handle);
        return -1;
    }

    // Status LED
    uint8_t status_led_gpio = 0;
    err = nvs_get_u8(lights_config_handle, "status_led", &status_led_gpio); 
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "error nvs_get_u8 status_led err %d", err);
        nvs_close(lights_config_handle);
        return -1;
    }
    led_status = led_status_init(status_led_gpio, invert[0] ? false : true);

    // used throughout main program
    light_gpio_inverted = invert[1];
    led_gpio_inverted = invert[2];

    // button configuration
    button_config_t button_config = {
        .active_level = invert[3] ? BUTTON_ACTIVE_LOW : BUTTON_ACTIVE_HIGH,
        .repeat_press_timeout = 300,
        .long_press_time = 10000,
    };

    // Get configured number of lights
    uint8_t num_lights = 0;
    err = nvs_get_u8(lights_config_handle, "num_lights", &num_lights);
    if (err != ESP_OK || num_lights == 0) {
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "error nvs_get_u8 num_lights err %d", err);
        }
        else {
            ESP_LOGE(TAG, "error no lights configured");
        }
        nvs_close(lights_config_handle);
        return -1;
    } 

    // From lights.h - NVS config blob
    lights_t light_config[num_lights];
    size = num_lights * sizeof(lights_t);
    err = nvs_get_blob(lights_config_handle, "config", light_config, &size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob config err %d ", err);
        nvs_close(lights_config_handle);
        return -1;
    }

    // if there are ONLY remote switches, then homekit should not be started
    uint8_t num_remote_lights = 0;   
  
    // create temporary arrays used for PWM configuration
    uint32_t pins[num_lights];
    uint32_t duties[num_lights];
    int16_t phases[num_lights];
    uint8_t i_pwm = 0;
    uint8_t pwm_invert_mask = 0;

    // status/feedback LEDs on each button  OR  standard ON/OFF output for Light (not PWM/dimming)
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 0; 

    for (uint8_t i = 0; i < num_lights; i++) {
        // create new light
        light_service_t *light = (light_service_t*)malloc(sizeof(light_service_t));
        memset(light, 0, sizeof(*light));

        // save NVS data to light config
        light->light_gpio = light_config[i].light_gpio; 
        light->led_gpio = light_config[i].led_gpio;
        light->is_dimmer = light_config[i].is_dimmer; 
        light->is_remote = light_config[i].is_remote; 
    
        // hardware button 
        err = button_create(light_config[i].button_gpio, button_config, button_callback, light);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "error button %d button_create err %d ", i, err);
            free(light);
            continue;
        }

        // button feedback LED 
        io_conf.pin_bit_mask = (1ULL<<light->led_gpio);
        err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "error button %d gpio_config err %d", i, err);
            free(light);
            continue;
        }
        gpio_set_level(light->led_gpio, led_gpio_inverted ? 1 : 0);

        if(light->is_remote) {
            // retrieve configured nvs command with 'host' and 'payload' keys
            int remote_cmd_len = snprintf(NULL, 0, "rem_cmd_%d", i);
            char *remote_cmd_key = malloc(remote_cmd_len + 1);
            snprintf(remote_cmd_key, remote_cmd_len + 1, "rem_cmd_%d", i);
            size_t required_size;
            err = nvs_get_str(lights_config_handle, remote_cmd_key, NULL, &required_size); //includes zero-terminator
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "error button %d nvs_get_str %s err %d", i, remote_cmd_key, err);
                free(remote_cmd_key);
                free(light);
                continue;
            }

            char *remote_cmd_val = malloc(required_size); 
            nvs_get_str(lights_config_handle, remote_cmd_key, remote_cmd_val, &required_size);
            
            // nvs configuration will be stored as cJSON for life of light
            light->nvs_command = cJSON_Parse(remote_cmd_val);

            free(remote_cmd_key);
            free(remote_cmd_val);

            // parse 'host' and 'payload' and validate
            cJSON *host_json = cJSON_GetObjectItem(light->nvs_command, "host");
            cJSON *payload_json = cJSON_GetObjectItem(light->nvs_command, "payload");

            if (!host_json || !payload_json) {
                ESP_LOGE(TAG, "error button %d key: \"host\" or \"payload\" not found", i);
                cJSON_Delete(light->nvs_command);
                free(light);
                continue;
            }
            else if (!cJSON_IsArray(payload_json)) {
                ESP_LOGE(TAG, "error button %d \"payload\" was not a valid json array", i);
                cJSON_Delete(light->nvs_command);
                free(light);
                continue;
            }

            // Remote can still support the dimming (BRIGHTNESS) characteristic
            //   create timer for long button hold - dimming function
            // 500ms per 5% (increment value is set in the remote_hk task) is 5s
            light->dim_timer = xTimerCreate(
                "dim_timer", pdMS_TO_TICKS(500), pdTRUE, light, light_dim_timer_callback
            );
            light->dim_direction = -1;

            num_remote_lights++;
        }
        else if (light->is_dimmer) {
            // PWM configuration - add to pins array ready for configuration. only required for hardware dimming
            pins[i_pwm] = light->light_gpio;
            duties[i_pwm] = 0;
            phases[i_pwm] = 0;
            pwm_invert_mask |= (light_gpio_inverted << i_pwm);

            // store PWM channel number and incrememnt
            light->pwm_channel = i_pwm;
            i_pwm++;

            // create timer for long button hold - dimming function
            // 100ms per 2% is 5s
            light->dim_timer = xTimerCreate(
                "dim_timer", pdMS_TO_TICKS(100), pdTRUE, light, light_dim_timer_callback
            );
            light->dim_direction = -1;
        }
        else {
            // Not PWM and not Remote. 
            io_conf.pin_bit_mask = (1ULL<<light->light_gpio);
            err = gpio_config(&io_conf);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "error button %d configuring gpio_config err %d", i, err);
                free(light);
                continue;
            }
            gpio_set_level(light->light_gpio, light_gpio_inverted ? 1 : 0);
        }

        light->next = lights;
        lights = light;
    }

    nvs_close(lights_config_handle);

    // configure PWM
    if (i_pwm > 0) {
        err  = pwm_init(PWM_PERIOD_IN_US, duties, i_pwm, pins);    
        err |= pwm_set_channel_invert(pwm_invert_mask);        // parameter is a bit mask
        err |= pwm_set_phases(phases);                         // throws an error if not set (even if it's 0)
        err |= pwm_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pwm config err %d ", err);
        return -1;
    }

    // configure task to manage remote commands
    if (num_remote_lights > 0) {
        // create the queue used to send complete struct remote_hk_t structures
        q_remotehk_message_queue = xQueueCreate(10, sizeof(remote_hk_t));
        xTaskCreate(&remote_hk_task, "remote_hk", 5120, NULL, 5, NULL);
    }

    ESP_LOGI(TAG, "num lights %d, PWM lights %d, num_remote_lights %d", num_lights, i_pwm, num_remote_lights);
 
    return num_lights - num_remote_lights;      // if > 0 then homekit server will start
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

    int8_t num_hardware_lights = configure_peripherals();
        
    if (num_hardware_lights == 0) {
        // no homekit accessory to be created. all lights are remote
        paired = true;
    }
    else if (num_hardware_lights > 0) {
        init_accessory();
        homekit_server_init(&config);
        paired = homekit_is_paired();
    }
    else {
        // indicate error
        paired = false;
    }

}
