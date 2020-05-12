#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <freertos/timers.h>
#include <sys/param.h>                          // MIN MAX

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "esp_ota_ops.h"
#include "esp_image_format.h"

#include "wifi.h"
#include "httpd.h"
#include <homekit/homekit.h>

#include "esp_log.h"
static const char *TAG = "myhttpd";

#define SCRATCH_BUFSIZE 1024

static httpd_handle_t server = NULL;

#ifdef CONFIG_IDF_TARGET_ESP32
    esp_event_handler_instance_t wifi_event_handler_instance;
    esp_event_handler_instance_t ip_event_handler_instance;
#endif

char log_buf[LOG_BUF_MAX_LINE_SIZE];
static TaskHandle_t t_sse_task_handle;
static QueueHandle_t q_sse_message_queue;
static putchar_like_t old_function = NULL;

// set volatile as a client could be removed at any time and the sse_logging_task 
// needs to make sure it has an updated copy
volatile int sse_sockets[MAX_SSE_CLIENTS];

int sse_logging_putchar(int chr) {
    if(chr == '\n'){
        // send without the '\n'
        xQueueSendToBack(q_sse_message_queue, log_buf, 0);
        // 'clear' string
        log_buf[0] = '\0';    
    } else {
        size_t len = strlen(log_buf);
        if (len < LOG_BUF_MAX_LINE_SIZE - 1) {
            log_buf[len] = chr;
            log_buf[len+1] = '\0';
        }
    }
    // still send to console
	return old_function( chr );
}


void send_sse_message (char* message, char* event) {
    const char *sse_data = "data: ";
    const char *sse_event = "\nevent: ";
    const char *sse_end_message = "\n\n";

    size_t event_len = 0;
    if (event != NULL) {
        event_len = strlen(sse_event) + strlen(event);
    }
    char send_buf[strlen(sse_data) + strlen(message) + event_len + strlen(sse_end_message)];
    strcpy(send_buf, sse_data);
    strcat(send_buf, message);
    if (event != NULL) {
        strcat(send_buf, sse_event);
        strcat(send_buf, event);
    }
    strcat(send_buf, sse_end_message);

    size_t message_len = strlen(send_buf);

    int return_code;
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (sse_sockets[i] != 0) {
            return_code = send(sse_sockets[i], send_buf, message_len, 0);
            if (return_code < 0) {
                httpd_sess_trigger_close(server, sse_sockets[i]);
            }
        }
    }
}

static void sse_logging_task(void * param)
{
    char recv_buf[LOG_BUF_MAX_LINE_SIZE];

    while(1) {
        if (xQueueReceive(q_sse_message_queue, recv_buf, portMAX_DELAY) == pdTRUE) {
            send_sse_message(recv_buf, NULL);
        } 
    }
}

static void status_json_sse_handler()
{
    char ip_buf[17];
    char *out;
    cJSON *root;
    root = cJSON_CreateObject();

    wifi_config_t wifi_cfg;
    esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_cfg);

    #ifdef CONFIG_IDF_TARGET_ESP32
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_get_ip_info(esp_netif, &ip_info)
        bool if_status = esp_netif_is_netif_up(esp_netif);
    #elif CONFIG_IDF_TARGET_ESP8266
        tcpip_adapter_ip_info_t ip_info;
        ESP_ERROR_CHECK(tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info));
        bool if_status = tcpip_adapter_is_netif_up(TCPIP_ADAPTER_IF_STA);
    #endif

    cJSON_AddItemToObject(root, "ssid", cJSON_CreateString((const char *)wifi_cfg.sta.ssid));
    snprintf(ip_buf, 17, IPSTR, IP2STR(&ip_info.ip));
    cJSON_AddItemToObject(root, "ip", cJSON_CreateString(ip_buf));
    snprintf(ip_buf, 17, IPSTR, IP2STR(&ip_info.netmask));
    cJSON_AddItemToObject(root, "netmask", cJSON_CreateString(ip_buf));
    snprintf(ip_buf, 17, IPSTR, IP2STR(&ip_info.gw));
    cJSON_AddItemToObject(root, "gw", cJSON_CreateString(ip_buf));
    cJSON_AddItemToObject(root, "if_status", cJSON_CreateBool(if_status));
 
    out = cJSON_PrintUnformatted(root);

    ESP_LOGI(TAG, "ssid: %s, ip:"IPSTR", netmask:"IPSTR", gw:"IPSTR", if_up? %s",
        wifi_cfg.sta.ssid, IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw), if_status?"TRUE":"FALSE");

    send_sse_message(out, "status");

    /* free all objects under root and root itself */
    cJSON_Delete(root);
    free(out);

}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    status_json_sse_handler();
}

void free_sse_ctx_func(void *ctx)
{
    int client_fd = *((int*) ctx);

    ESP_LOGD(TAG, "free_sse_context sse_socket: %d", client_fd);

    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (sse_sockets[i] == client_fd) {
            sse_sockets[i] = 0;
        }
    } 
    free(ctx);
}

esp_err_t server_side_sevent_registration_handler(httpd_req_t *req)
{
    // disable sending to sse_socket until a proper HTTP 200 OK response has been sent back to client
    old_function = esp_log_set_putchar(old_function);

    int len;
    char buffer[150];
    int i = 0;

    /* Create session's context if not already available */
    if (!req->sess_ctx) {
        for (i = 0; i < MAX_SSE_CLIENTS; i++) {
            if (sse_sockets[i] == 0) {
                req->sess_ctx = malloc(sizeof(int)); 
                req->free_ctx = free_sse_ctx_func; 
                int client_fd = httpd_req_to_sockfd(req);  
                *(int *)req->sess_ctx = client_fd;
                sse_sockets[i] = client_fd;
                ESP_LOGD(TAG, "sse_socket: %d slot %d", sse_sockets[i], i);
                break;
            }
        }
        if (i == MAX_SSE_CLIENTS) {
            len = sprintf(buffer, "HTTP/1.1 503 Server Busy\r\nContent-Length: 0\r\n\r\n");
        }
        else {
            len = sprintf(buffer, "HTTP/1.1 200 OK\r\n"
                                    "Connection: Keep-Alive\r\n"
                                    "Content-Type: text/event-stream\r\n"
                                    "Cache-Control: no-cache\r\n\r\n");
        }
    } else {
        // Should never get here?
        len = sprintf(buffer, "HTTP/1.1 400 Session Already Active\r\n\r\n");
    }

    // need to use raw send function, as httpd_resp_send will add Content-Length: 0 which
    // will make the client disconnect
    httpd_send(req, buffer, len);

    // send first wi-fi status information;
    status_json_sse_handler();

    // send firmware details information;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);
    sprintf(buffer, "{\"version\":\"%s\"}", app_desc.version);
    send_sse_message(buffer, "firmware");

    // enable sse logging again
    old_function = esp_log_set_putchar(old_function);  

    return ESP_OK;
}

esp_err_t root_handler(httpd_req_t *req)
{
    extern const char index_html_start[] asm("_binary_wifi_html_gz_start");
    extern const char index_html_end[] asm("_binary_wifi_html_gz_end");

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, index_html_start ,(index_html_end-index_html_start));

    return ESP_OK;
}

esp_err_t ap_json_handler(httpd_req_t *req)
{
    char *out;
    cJSON *root, *fld;
    root = cJSON_CreateArray();

    uint16_t ap_count = 0;

    if( xSemaphoreTake(*(get_wifi_mutex()), 500/portTICK_PERIOD_MS) == pdTRUE) {
        esp_wifi_scan_start(NULL, true);
        xSemaphoreGive(*(get_wifi_mutex()));       // only if wifi_scan_start is blocking, otherwise, do it in SCAN_DONE event

        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

        wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_info));

        ESP_LOGI(TAG, "Total APs scanned = %u", ap_count);
        for (int i = 0; i < ap_count && i <= MAX_AP_COUNT; i++) {
            cJSON_AddItemToArray(root, fld = cJSON_CreateObject());
            cJSON_AddItemToObject(fld, "ssid", cJSON_CreateString((const char *)ap_info[i].ssid));
            cJSON_AddItemToObject(fld, "chan", cJSON_CreateNumber(ap_info[i].primary));
            cJSON_AddItemToObject(fld, "rssi", cJSON_CreateNumber(ap_info[i].rssi));
            cJSON_AddItemToObject(fld, "auth", cJSON_CreateNumber(ap_info[i].authmode));
        }
        free(ap_info);
    }
    else {
        ESP_LOGI(TAG, "Scan or connect in progress");
    }

    out = cJSON_PrintUnformatted(root);
   
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_send(req, out, strlen(out));      

    /* free all objects under root and root itself */
    cJSON_Delete(root);
    free(out);

    return ESP_OK;
}


esp_err_t connect_json_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char buf[SCRATCH_BUFSIZE];
    int received = 0;

    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        #ifdef CONFIG_IDF_TARGET_ESP32
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        #elif CONFIG_IDF_TARGET_ESP8266
            httpd_resp_send_500(req);
        #endif
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            #ifdef CONFIG_IDF_TARGET_ESP32            
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error receiving data");
            #elif CONFIG_IDF_TARGET_ESP8266
                httpd_resp_send_500(req);
            #endif
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pwd = cJSON_GetObjectItem(root, "password");

    wifi_config_t wifi_cfg;
    esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_cfg);

    snprintf((char *)wifi_cfg.sta.ssid, 32, "%s", ssid->valuestring);
    snprintf((char *)wifi_cfg.sta.password, 32, "%s", pwd->valuestring);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg)); 

    esp_wifi_disconnect();  
    esp_wifi_connect();         // ignore mutex - we want this to take priority and immediately
    
    httpd_resp_send(req, NULL, 0);

    cJSON_Delete(root);
    return ESP_OK;
}

static void restart_timer_callback(TimerHandle_t timer) {
    esp_restart();
}

esp_err_t restart_json_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char buf[SCRATCH_BUFSIZE];
    int received = 0;

    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        #ifdef CONFIG_IDF_TARGET_ESP32
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        #elif CONFIG_IDF_TARGET_ESP8266
            httpd_resp_send_500(req);
        #endif
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            #ifdef CONFIG_IDF_TARGET_ESP32            
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error receiving data");
            #elif CONFIG_IDF_TARGET_ESP8266
                httpd_resp_send_500(req);
            #endif
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *reset_nvs = cJSON_GetObjectItem(root, "reset-nvs");
    cJSON *reset_homekit = cJSON_GetObjectItem(root, "reset-homekit");
 
    if (cJSON_IsTrue(reset_nvs)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
    }
    if (cJSON_IsTrue(reset_homekit)) {
        homekit_server_reset();
    }

    //start Timer to restart.
    TimerHandle_t restart_timer;
    restart_timer = xTimerCreate("Restart Timer", pdMS_TO_TICKS(2000),
            pdFALSE, NULL, restart_timer_callback
        );
    xTimerStart(restart_timer, 1);

    ESP_LOGW(TAG, "Reset nvs = %d, homekit = %d. Restarting...", cJSON_IsTrue(reset_nvs), cJSON_IsTrue(reset_homekit));

    // Just send an OK response. Client can monitor console logs.
    httpd_resp_send(req, NULL, 0);

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t update_json_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char buf[SCRATCH_BUFSIZE];
    int received = 0;

    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        #ifdef CONFIG_IDF_TARGET_ESP32
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        #elif CONFIG_IDF_TARGET_ESP8266
            httpd_resp_send_500(req);
        #endif
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            #ifdef CONFIG_IDF_TARGET_ESP32            
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error receiving data");
            #elif CONFIG_IDF_TARGET_ESP8266
                httpd_resp_send_500(req);
            #endif
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *update = cJSON_GetObjectItem(root, "update");
 
    if (cJSON_IsTrue(update)) {
        const esp_partition_t *app0_partition;
        app0_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);

        if (esp_ota_set_boot_partition(app0_partition) != ESP_OK) {
            ESP_LOGW(TAG, "Unable to set boot partition to '%s' at offset 0x%x",
                            app0_partition->label, app0_partition->address);
        } else {
            //start Timer to restart.
            TimerHandle_t restart_timer;
            restart_timer = xTimerCreate("Restart Timer", pdMS_TO_TICKS(2000),
                    pdFALSE, NULL, restart_timer_callback
                );
            xTimerStart(restart_timer, 1);

            ESP_LOGW(TAG, "Booting to '%s' at offset 0x%x. Restarting...",
                             app0_partition->label, app0_partition->address);
        }
    }
    else {
        ESP_LOGW(TAG, "'update' URI called, but JSON incorrect");
    }
    // Just send an OK response. Client can monitor console logs.
    httpd_resp_send(req, NULL, 0);

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t update_boot_handler(httpd_req_t *req)
{
    char buffer[SCRATCH_BUFSIZE];
    int len = 0;
               
    esp_ota_handle_t ota_handle; 
    esp_err_t err = ESP_OK;

    const esp_partition_t *boot_partition;
    boot_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (boot_partition == NULL) {
        err = ESP_ERR_OTA_SELECT_INFO_INVALID;
    }

    // File cannot be larger than partition size
    if (boot_partition != NULL && req->content_len > boot_partition->size) {
        ESP_LOGE(TAG, "Content-Length of %d larger than partition size of %d", req->content_len, boot_partition->size); 
        err = ESP_ERR_INVALID_SIZE;
    }

    int remaining = req->content_len;
    bool is_image_header_checked = false;
    bool more_content = true;

    while (more_content) {
        if (err == ESP_OK && len > sizeof(esp_image_header_t) && !is_image_header_checked) {
            esp_image_header_t check_header; 
            memcpy(&check_header, buffer, sizeof(esp_image_header_t));
            int32_t entry = check_header.entry_addr - 0x40200010;

            if (check_header.magic != 0xE9) {
                ESP_LOGE(TAG, "OTA image has invalid magic byte (expected 0xE9, saw 0x%x)", check_header.magic);
                err = ESP_ERR_OTA_VALIDATE_FAILED;
            }
            else if ( (entry < boot_partition->address)  ||
                (entry > boot_partition->address + boot_partition->size) ) {
                ESP_LOGE(TAG, "OTA binary start entry 0x%x, partition start from 0x%x to 0x%x", entry, 
                    boot_partition->address, boot_partition->address + boot_partition->size);
                err = ESP_ERR_OTA_VALIDATE_FAILED;
            }

            if (err == ESP_OK) {
                // esp_ota_begin erases the partition, so only call it if the incoming file 
                //  looks OK.
                err = esp_ota_begin(boot_partition, OTA_SIZE_UNKNOWN, &ota_handle);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Error esp_ota_begin()"); 
                } 
                else {
                    ESP_LOGI(TAG, "Writing to partition '%s' at offset 0x%x",
                        boot_partition->label, boot_partition->address);
                }
            }
            is_image_header_checked = true;
        }

        // if no previous errors, continue to write. otherwise, just read the incoming data until it completes
        //  and send the HTTP error response back. stopping the connection early causes the client to
        //  display an ERROR CONNECTION CLOSED response instead of a meaningful error
        if (err == ESP_OK && is_image_header_checked) {
            err = esp_ota_write(ota_handle, buffer, len);
        }

        remaining -= len;

        if (remaining != 0) {
            if ((len = httpd_req_recv(req, buffer, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
                if (len == HTTPD_SOCK_ERR_TIMEOUT) {
                    /* Retry if timeout occurred */
                    len = 0;
                    continue;
                }
                ESP_LOGE(TAG, "File reception failed!");
                err = ESP_FAIL;
            }
        }
        else {
            more_content = false;
        }
    } // end while(). no more content to read

    ESP_LOGI(TAG, "Binary transferred finished: %d bytes", req->content_len);

    if (ota_handle) {
        err = esp_ota_end(ota_handle);
    } 

    if (err == ESP_OK) {
        httpd_resp_send(req, NULL, 0);
    } else {
        #ifdef CONFIG_IDF_TARGET_ESP32            
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA Failed");
        #elif CONFIG_IDF_TARGET_ESP8266
            httpd_resp_set_status(req, HTTPD_400);
            httpd_resp_send(req, NULL, 0);
        #endif
    }
    
    return ESP_OK;
}

esp_err_t start_webserver(void)
{

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 5;
    // kick off any old socket connections to allow new connections
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");

        httpd_uri_t root_page = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root_page);

        // Scan AP. Requested when the refresh button is pressed
        httpd_uri_t ap_json_page = {
            .uri       = "/ap.json",
            .method    = HTTP_GET,
            .handler   = ap_json_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &ap_json_page);

        // Sends SSID and password for AP to connect to
        httpd_uri_t connect_json_page = {
            .uri       = "/connect.json",
            .method    = HTTP_POST,
            .handler   = connect_json_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &connect_json_page);

        // Requests a restart - includes JSON with whether to reset NVS and/or HomeKit
        httpd_uri_t restart_json_page = {
            .uri       = "/restart.json",
            .method    = HTTP_POST,
            .handler   = restart_json_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &restart_json_page);

        // Requests ESP to restart to 'bootloader' partition to begin update of the 'main' partition
        httpd_uri_t update_json_page = {
            .uri       = "/update.json",
            .method    = HTTP_POST,
            .handler   = update_json_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &update_json_page);

        // CLient requesting Server Side Events
        httpd_uri_t server_side_sevent_registration_page = {
            .uri       = "/event",
            .method    = HTTP_GET,
            .handler   = server_side_sevent_registration_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &server_side_sevent_registration_page);

        // Not shown on the webpage. Used to update boot partition
        httpd_uri_t update_boot_page = {
            .uri       = "/updateboot",
            .method    = HTTP_POST,
            .handler   = update_boot_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &update_boot_page);

        // esp_event_handler_register is being deprecated
        #ifdef CONFIG_IDF_TARGET_ESP32
            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &wifi_event_handler_instance));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &ip_event_handler_instance));
        #elif CONFIG_IDF_TARGET_ESP8266
            ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
            ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
        #endif
        
        // Task to accept messages from queue and send to SSE clients
        q_sse_message_queue = xQueueCreate( 10, sizeof(char)*LOG_BUF_MAX_LINE_SIZE );
        xTaskCreate(&sse_logging_task, "sse", 2048, NULL, 4, &t_sse_task_handle);

        old_function = esp_log_set_putchar(&sse_logging_putchar);

        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return ESP_FAIL;
}

void stop_webserver(void)
{
    // esp_event_handler_register is being deprecated
    #ifdef CONFIG_IDF_TARGET_ESP32
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler_instance));
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler_instance));
    #elif CONFIG_IDF_TARGET_ESP8266
        ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler));
        ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler));
    #endif
    
    esp_log_set_putchar(old_function);
    vTaskDelete(t_sse_task_handle);
    vQueueDelete(q_sse_message_queue);

    // Stop the httpd server
    httpd_stop(server);
}


