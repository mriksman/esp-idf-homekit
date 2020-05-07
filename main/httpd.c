#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <freertos/timers.h>

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "wifi.h"
#include "httpd.h"
#include <homekit/homekit.h>

#include "esp_log.h"
static const char *TAG = "myhttpd";

#define SCRATCH_BUFSIZE 1024

static httpd_handle_t server = NULL;

#define LOG_BUF_MAX_LINE_SIZE 120
char log_buf[LOG_BUF_MAX_LINE_SIZE];
static QueueHandle_t q_sse_message_queue;
static putchar_like_t old_function = NULL;

// set volatile as a client could be removed at any time and the sse_task needs to make sure it has an updated copy
#define MAX_SSE_CLIENTS 2
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

static void sse_task(void * param)
{
    const char *sse_begin = "data: ";
    const char *sse_end_message = "\n\n";
    char recv_buf[LOG_BUF_MAX_LINE_SIZE + strlen(sse_begin) + strlen(sse_end_message)];
    strcpy(recv_buf, sse_begin);
    int return_code;

    while(1) {
        if (xQueueReceive(q_sse_message_queue, recv_buf + strlen(sse_begin), portMAX_DELAY) == pdTRUE) {
            strcat(recv_buf, sse_end_message);
            for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
                if (sse_sockets[i] != 0) {
                    return_code = send(sse_sockets[i], recv_buf, strlen(recv_buf), 0);
                    if (return_code < 0) {
                         httpd_sess_trigger_close(server, sse_sockets[i]);
                    }
                }
            } //for
        } // if
    } //while
}

void free_sse_ctx_func(void *ctx)
{
    int client_fd = *((int*) ctx);

    ESP_LOGI(TAG, "FREE sse_socket: %d", client_fd);

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

    int client_fd = httpd_req_to_sockfd(req);
    int len;
    char buffer[150];
    int i = 0;

    /* Create session's context if not already available */
    if (!req->sess_ctx) {
        for (i = 0; i < MAX_SSE_CLIENTS; i++) {
            if (sse_sockets[i] == 0) {
                req->sess_ctx = malloc(sizeof(int)); 
                req->free_ctx = free_sse_ctx_func;   
                *(int *)req->sess_ctx = client_fd;
                sse_sockets[i] = client_fd;
                ESP_LOGI(TAG, "sse_socket: %d slot %d pointer sess_ctx %p", sse_sockets[i], i, req->sess_ctx);
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


    // need to send raw data, as httpd_resp_send will add Content-Length: 0 which
    // will make the client disconnect
    httpd_send(req, buffer, len);

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

    out = cJSON_Print(root);
   
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
    char resp[8];

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

    ESP_LOGI(TAG, "SSID = %s, Password = %s", ssid->valuestring, pwd->valuestring);

    wifi_config_t wifi_cfg;
    esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_cfg);

    snprintf((char *)wifi_cfg.sta.ssid, 32, "%s", ssid->valuestring);
    snprintf((char *)wifi_cfg.sta.password, 32, "%s", pwd->valuestring);

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg)); 

    esp_wifi_disconnect();  
    esp_wifi_connect();         // ignore mutex - we want this to take priority and immediately
    
    snprintf(resp, 8, "OK");
    httpd_resp_send(req, resp, strlen(resp));

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
    char msg[60];

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
    restart_timer = xTimerCreate("Restart Timer", pdMS_TO_TICKS(5000),
            pdFALSE, NULL, restart_timer_callback
        );
    xTimerStart(restart_timer, 1);

    snprintf(msg, 60, "Reset nvs = %d, homekit = %d. Restarting in 5 seconds...", cJSON_IsTrue(reset_nvs), cJSON_IsTrue(reset_homekit));
    ESP_LOGW(TAG, msg);
    httpd_resp_send(req, msg, strlen(msg));

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t status_json_handler(httpd_req_t *req)
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

 
    out = cJSON_Print(root);

    ESP_LOGI(TAG, "ssid: %s, ip:"IPSTR", netmask:"IPSTR", gw:"IPSTR", if_up? %s",
        wifi_cfg.sta.ssid, IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw), if_status?"TRUE":"FALSE");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_send(req, out, strlen(out));      

    /* free all objects under root and root itself */
    cJSON_Delete(root);
    free(out);

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

        httpd_uri_t ap_json_page = {
            .uri       = "/ap.json",
            .method    = HTTP_GET,
            .handler   = ap_json_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &ap_json_page);

        httpd_uri_t status_json_page = {
            .uri       = "/status.json",
            .method    = HTTP_GET,
            .handler   = status_json_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &status_json_page);

        httpd_uri_t connect_json_page = {
            .uri       = "/connect.json",
            .method    = HTTP_POST,
            .handler   = connect_json_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &connect_json_page);

        httpd_uri_t restart_json_page = {
            .uri       = "/restart.json",
            .method    = HTTP_POST,
            .handler   = restart_json_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &restart_json_page);

        httpd_uri_t server_side_sevent_registration_page = {
            .uri       = "/event",
            .method    = HTTP_GET,
            .handler   = server_side_sevent_registration_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &server_side_sevent_registration_page);

        // Task to accept messages from queue and send to SSE clients
        xTaskCreate(&sse_task, "sse", 2048, NULL, 4, NULL);
        q_sse_message_queue = xQueueCreate( 10, sizeof(char)*LOG_BUF_MAX_LINE_SIZE );

        old_function = esp_log_set_putchar(&sse_logging_putchar);


        return ESP_OK;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return ESP_FAIL;
}

void stop_webserver(void)
{
    // Stop the httpd server
    httpd_stop(server);
}


