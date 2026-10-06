#include <Arduino.h>
#include "esp_task_wdt.h"
#include "watchdog.h"

// แก้จาก 10 -> 30 วินาที กัน reset ระหว่างต่อ WiFi / MQTT
#define WATCHDOG_TIMEOUT_SEC 30


void setupWatchdog()
{
    Serial.println("[WDT] Initializing Watchdog...");

    esp_err_t result = esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);

    if (result == ESP_OK)
    {
        Serial.println("[WDT] Watchdog initialized");
    }
    else if (result == ESP_ERR_INVALID_STATE)
    {
        Serial.println("[WDT] Watchdog already initialized");
    }
    else
    {
        Serial.print("[WDT] Init error = ");
        Serial.println(result);
    }

    esp_err_t addResult = esp_task_wdt_add(NULL);

    if (addResult == ESP_OK || addResult == ESP_ERR_INVALID_STATE)
    {
        Serial.println("[WDT] Task registered");
    }
    else
    {
        Serial.print("[WDT] Task registration error = ");
        Serial.println(addResult);
    }
}


void feedWatchdog()
{
    esp_task_wdt_reset();
}