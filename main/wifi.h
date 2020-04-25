#pragma once

#ifdef __cplusplus
extern "C" {
#endif

SemaphoreHandle_t* get_wifi_mutex();

void wifi_init();
void start_ap_prov();



esp_err_t wifi_process_event(void *ctx, system_event_t *event);




#ifdef __cplusplus
}
#endif