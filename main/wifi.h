#pragma once

#ifdef __cplusplus
extern "C" {
#endif

SemaphoreHandle_t* get_wifi_mutex();

void wifi_init(void);
void start_ap_prov();

#ifdef __cplusplus
}
#endif