#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

#include "ota_manager.h"
#include "watchdog.h"

// แก้จาก ESP32_LowPower -> ใช้ "-" แทน "_" เพื่อให้ mDNS resolve ได้ง่าย
#define OTA_HOSTNAME "esp32-lowpower"
#define OTA_PASSWORD "ESP32OTA"      // ต้องตรงกับ --auth ใน platformio.ini


void setupOTA()
{
    Serial.println("[OTA] Initializing OTA...");

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]()
    {
        Serial.println();
        Serial.println("[OTA] Update started");
        feedWatchdog();
    });

    ArduinoOTA.onEnd([]()
    {
        Serial.println();
        Serial.println("[OTA] Update completed");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
    {
        feedWatchdog();
        Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
    });

    ArduinoOTA.onError([](ota_error_t error)
    {
        Serial.printf("\n[OTA] Error[%u]\n", error);
        feedWatchdog();
    });

    ArduinoOTA.begin();

    Serial.println("[OTA] Ready");
    Serial.print("[OTA] Hostname = ");
    Serial.println(OTA_HOSTNAME);
    Serial.print("[OTA] IP = ");
    Serial.println(WiFi.localIP());
}


void handleOTA()
{
    ArduinoOTA.handle();
    feedWatchdog();
}