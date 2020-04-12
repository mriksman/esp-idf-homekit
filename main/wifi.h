#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_event.h"                          // For esp_event_base_t

void wifi_init(void);

// Declare an event base
ESP_EVENT_DECLARE_BASE(CUSTOM_WIFI_EVENT);

enum {                                       // declaration of the specific events 
    START_WIFI_SCAN, 
    START_WIFI_CONNECT
};

#ifdef __cplusplus
}
#endif