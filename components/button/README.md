esp-idf-button
==========
Library for [ESP-IDF] (including ESP8266 RTOS SDK > v3.0) to handle
button input.

Based heavily on https://github.com/maximkulkin/esp-button

Before you start using library, you need to figure out how button is/will be wired.
There are two ways to wire button:
* active high - signal goes from low to high when button is pressed
* active low - signal connects to ground when button is pressed

Pull-ups and pull-downs are not enabled by default. If they are required, call either
```
gpio_set_pull_mode(gpio_num, GPIO_PULLUP_ONLY)
gpio_set_pull_mode(gpio_num, GPIO_PULLDOWN_ONLY)
```
after the button is created.
Note: ESP8266
* only GPIO0-15 can be pulled up internally
* only GPIO16 can be pulled down internally (can be pull-up externally)

On Dev Board (like NodeMCU v3)
* GPIO15 is pulled low externally 
* GPIO0 and GPIO2 are pulled high externally

Button config settings:
* **active_level** - `BUTTON_ACTIVE_HIGH` or `BUTTON_ACTIVE_LOW` - which signal level corresponds to button press. In case of `BUTTON_ACTIVE_LOW`, it automatically enables pullup resistor on button pin. In case of `BUTTON_ACTIVE_HIGH`, you need to have an additional pulldown (pin-to-ground) resistor on button pin.
* **repeat\_press_time** - defines maximum time in milliseconds to wait for subsequent press to consider it a multiple press (defaults to 300ms).
* **long\_press_time** - defines time in milliseconds for press to be considered a long press. If not configured, a timer will not be created (saving resources).

Implementation effectively handles debounce, no additional configuration is required.

Example of usage

```c
#include <button.h>

#define BUTTON_PIN 5

void button_callback(button_event_t event, void* context) {
    uint8_t button_idx = *((uint8_t*) context);

    if (event_id == BUTTON_EVENT_DOWN) {
        ESP_LOGI(TAG, "button %d down", button_idx);
    }
    else if (event_id == BUTTON_EVENT_UP) {
        ESP_LOGI(TAG, "button %d up", button_idx);
    }

    else if (event_id == BUTTON_EVENT_DOWN_HOLD) {
        ESP_LOGI(TAG, "button %d being held down", button_idx);
    }
    else if (event_id == BUTTON_EVENT_UP_HOLD) {
        ESP_LOGI(TAG, "button %d released from being held down", button_idx);
    }

    else if (event_id == BUTTON_EVENT_LONG_PRESS) {
        ESP_LOGI(TAG, "button %d long press", button_idx);
    }
    else {
        if (event_id == 1) {
            ESP_LOGI(TAG, "button %d pressed once", button_idx);
        } 
        else if (event_id == 2) {
            ESP_LOGI(TAG, "button %d pressed twice", button_idx);
        } 
    }
}

button_config_t button_config = BUTTON_CONFIG(
    BUTTON_ACTIVE_LOW,
    .repeat_press_timeout = 300,
    .long_press_time = 10000,
);

int button_idx = 1;

int r = button_create(BUTTON_PIN, config, button_callback, &button_idx);
if (r) {
    printf("Failed to initialize a button\n");
}
```

License
=======
MIT licensed. See the bundled [LICENSE](https://github.com/maximkulkin/esp-button/blob/master/LICENSE) file for more details.
