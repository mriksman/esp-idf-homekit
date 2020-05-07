#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_http_server.h>

esp_err_t start_webserver(void);
void stop_webserver(void);

#define MAX_AP_COUNT 10

#ifdef __cplusplus
}
#endif 