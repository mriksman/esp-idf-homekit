#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MAXIMUM_RETRY               6

SemaphoreHandle_t* get_wifi_mutex();

void wifi_init();
void start_ap_prov();

#ifdef __cplusplus
}
#endif