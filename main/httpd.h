#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_http_server.h>

httpd_handle_t start_webserver(void);
void stop_webserver(httpd_handle_t server);

#define MAX_AP_COUNT 10

#ifdef __cplusplus
}
#endif 