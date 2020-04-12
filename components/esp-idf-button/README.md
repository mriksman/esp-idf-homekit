esp-idf-button
==========
Library for [ESP-IDF] (including ESP8266 RTOS SDK > v3.0) to handle
button input.

Based heavily on https://github.com/maximkulkin/esp-button

Before you start using library, you need to figure out how button is/will be wired.
There are two ways to wire button:
* active high - signal goes from low to high when button is pressed

* active low - signal connects to ground when button is pressed

```c
#include <button.h>

#define BUTTON_PIN 5

void button_callback(button_event_t event, void* context) {
    printf("button press\n");
}

button_config_t config = BUTTON_CONFIG(button_active_high);

int r = button_create(BUTTON_PIN, config, button_callback, NULL);
if (r) {
    printf("Failed to initialize a button\n");
}
```

Button config settings:
* **active_level** - `button_active_high` or `button_active_low` - which signal level corresponds to button press. In case of `button_active_low`, it automatically enables pullup resistor on button pin. In case of `button_active_high`, you need to have an additional pulldown (pin-to-ground) resistor on button pin.
* **repeat\_press_time** - defines maximum time in milliseconds to wait for subsequent press to consider it a multiple press (defaults to 300ms).

Implementation effectively handles debounce, no additional configuration is required.

Example of using button with support of single, double and tripple presses:

```c
#include <button.h>

#define BUTTON_PIN 5

void button_callback(button_event_t event, void* context) {
    int button_idx = *((uint8_t*) context);

    switch (event) {
        case button_event_down:
            // Can start timers here to determine 'long press' (if required)
            ESP_LOGI(TAG, "button %d down", button_idx);
            break;
        case button_event_up:
            ESP_LOGI(TAG, "button %d up", button_idx);
            break;
        default:
            ESP_LOGI(TAG, "button %d pressed %d times", button_idx, event);
    }
}

button_config_t config = BUTTON_CONFIG(
    button_active_high,
);

int r = button_create(BUTTON_PIN, config, button_callback, NULL);
if (r) {
    printf("Failed to initialize a button\n");
}
```

License
=======
MIT licensed. See the bundled [LICENSE](https://github.com/maximkulkin/esp-button/blob/master/LICENSE) file for more details.
