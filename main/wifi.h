#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_event.h"                          // For esp_event_base_t

//#define WIFI_SSID "VMD72AA32"
//#define WIFI_PASSWORD "cBswrprc8b4A"
#define WIFI_SSID "Mike iPhone 11 Pro"
#define WIFI_PASSWORD "gambit18"

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