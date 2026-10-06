#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_sleep.h"

#include "watchdog.h"
#include "ota_manager.h"


// =====================================================
// Wi-Fi Configuration
// =====================================================

constexpr const char* WIFI_SSID = "OPPO A92JR";
constexpr const char* WIFI_PASSWORD = "0000000000";

// ให้เวลาต่อ Wi-Fi 30 วินาทีต่อครั้ง
constexpr uint32_t WIFI_TIMEOUT_MS = 30000;


// =====================================================
// ThingsBoard Configuration
// =====================================================

constexpr const char* TB_SERVER = "thingsboard.cloud";
constexpr uint16_t TB_PORT = 1883;

// แนะนำ: สร้าง token ใหม่ใน ThingsBoard เพราะ token เดิมถูกแชร์ไปแล้ว
constexpr const char* TB_TOKEN = "aYmhRsSR2rdt3QTeCy9O";


// =====================================================
// Deep Sleep
// =====================================================

constexpr uint64_t SLEEP_TIME_US = 30ULL * 1000000ULL;   // หลับ 30 วินาที


// =====================================================
// OTA
// =====================================================

// (สำรอง) เวลา OTA window แบบปกติ — โหมดปกติไม่เปิด OTA window แล้ว
constexpr uint32_t OTA_WINDOW_MS = 20000;

// OTA mode ที่สั่งจาก ThingsBoard: ค้างรอได้สูงสุด 3 นาที
constexpr uint32_t OTA_MODE_MAX_MS = 3UL * 60UL * 1000UL;

// true = ค้างรอ OTA ตลอดไป ไม่หลับ (ใช้ตอนพัฒนาเท่านั้น)
constexpr bool OTA_WAIT_INDEFINITELY = false;

// กดปุ่ม BOOT (GPIO 0) ค้างตอนบูต = ค้างรอ OTA ไม่หลับ
constexpr int OTA_BUTTON_PIN = 0;
bool forceOTA = false;

// สั่งผ่าน ThingsBoard shared attribute "ota_mode"
volatile bool otaModeRequested = false;

// ได้รับคำตอบจาก ThingsBoard แล้วหรือยัง
volatile bool tbReplied = false;


// =====================================================
// MQTT
// =====================================================

WiFiClient espClient;
PubSubClient mqttClient(espClient);


// รับคำสั่งจาก ThingsBoard (attribute update / attribute response)
void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    String msg;
    for (unsigned int i = 0; i < length; i++)
    {
        msg += (char)payload[i];
    }
    msg.replace(" ", "");

    Serial.print("[MQTT] RX: ");
    Serial.println(msg);

    tbReplied = true;

    if (msg.indexOf("\"ota_mode\":true") >= 0)
    {
        otaModeRequested = true;
        Serial.println("[OTA] ota_mode = TRUE");
    }
    else if (msg.indexOf("\"ota_mode\":false") >= 0)
    {
        otaModeRequested = false;
        Serial.println("[OTA] ota_mode = FALSE");
    }
}


// =====================================================
// Sensors
// =====================================================

float readTemperature()
{
    return temperatureRead();
}

int readMagneticField()
{
    return hallRead();   // ใช้ได้เฉพาะ Arduino core 2.x
}


// =====================================================
// เชื่อมต่อ Wi-Fi
// =====================================================

bool connectWiFi()
{
    Serial.println();
    Serial.println("[WIFI] Starting radio");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(500);

    for (int attempt = 1; attempt <= 3; attempt++)
    {
        Serial.print("[WIFI] Attempt ");
        Serial.print(attempt);
        Serial.println("/3");

        feedWatchdog();

        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

        uint32_t startMs = millis();

        while (WiFi.status() != WL_CONNECTED &&
               millis() - startMs < WIFI_TIMEOUT_MS)
        {
            feedWatchdog();
            delay(500);
            Serial.print(".");
        }

        Serial.println();

        if (WiFi.status() == WL_CONNECTED)
        {
            WiFi.setSleep(false);
            Serial.println("[WIFI] Connected");

            Serial.print("[WIFI] IP = ");
            Serial.println(WiFi.localIP());

            Serial.print("[WIFI] RSSI = ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");

            feedWatchdog();
            return true;
        }

        Serial.println("[WIFI] Connection failed");
        Serial.print("[WIFI] Status = ");
        Serial.println(WiFi.status());

        WiFi.disconnect();
        delay(1000);
        feedWatchdog();
    }

    Serial.println();
    Serial.println("[ERROR] Wi-Fi connection failed after 3 attempts");
    return false;
}


// =====================================================
// เชื่อมต่อ ThingsBoard
// =====================================================

bool connectThingsBoard()
{
    Serial.println();
    Serial.println("[MQTT] Connecting to ThingsBoard...");

    mqttClient.setServer(TB_SERVER, TB_PORT);
    mqttClient.setSocketTimeout(5);      // กัน connect ค้างนานเกิน watchdog
    mqttClient.setBufferSize(512);       // payload มี ip_address เพิ่มมา
    mqttClient.setCallback(mqttCallback);

    feedWatchdog();

    bool ok = mqttClient.connect("ESP32_LowPower", TB_TOKEN, NULL);

    feedWatchdog();

    if (ok)
    {
        Serial.println("[MQTT] ThingsBoard Connected");

        // ฟังคำสั่งใหม่ + ถามค่า ota_mode ปัจจุบัน
        mqttClient.subscribe("v1/devices/me/attributes");
        mqttClient.subscribe("v1/devices/me/attributes/response/+");
        mqttClient.publish("v1/devices/me/attributes/request/1",
                           "{\"sharedKeys\":\"ota_mode\"}");

        return true;
    }

    Serial.print("[ERROR] MQTT connection failed, state = ");
    Serial.println(mqttClient.state());
    return false;
}


// =====================================================
// Edge Processing
// =====================================================

float processTemperature(float temperature)
{
    if (temperature < -40 || temperature > 125)
    {
        Serial.println("[WARNING] Invalid temperature");
        return NAN;
    }

    return round(temperature * 10.0) / 10.0;
}

int processMagneticField(int magnetic)
{
    if (magnetic < -1000 || magnetic > 1000)
    {
        Serial.println("[WARNING] Invalid magnetic field");
        return 0;
    }

    return magnetic;
}


// =====================================================
// ส่ง Telemetry ไป ThingsBoard
// =====================================================

bool sendTelemetry(float temperature, int magneticField)
{
    if (!mqttClient.connected())
    {
        Serial.println("[ERROR] MQTT not connected");
        return false;
    }

    int rssi = WiFi.RSSI();
    uint32_t freeHeap = ESP.getFreeHeap();

    String payload = "{";
    payload += "\"temperature\":" + String(temperature, 1) + ",";
    payload += "\"magnetic_field\":" + String(magneticField) + ",";
    payload += "\"rssi_dbm\":" + String(rssi) + ",";
    payload += "\"free_heap\":" + String(freeHeap) + ",";
    payload += "\"ip_address\":\"" + WiFi.localIP().toString() + "\",";
    payload += "\"wifi_status\":\"CONNECTED\",";
    payload += "\"device_status\":\"ONLINE\"";
    payload += "}";

    Serial.println();
    Serial.println("[MQTT] Sending telemetry:");
    Serial.println(payload);

    bool success = mqttClient.publish("v1/devices/me/telemetry",
                                      payload.c_str());

    if (success)
        Serial.println("[MQTT] Telemetry sent successfully");
    else
        Serial.println("[ERROR] Telemetry send failed");

    return success;
}


// =====================================================
// แสดงข้อมูล Telemetry
// =====================================================

void showTelemetry(float temperature, int magneticField)
{
    Serial.println();
    Serial.println("================================");
    Serial.println("[TELEMETRY]");
    Serial.println("================================");

    Serial.print("temperature_c = ");
    Serial.println(temperature, 1);

    Serial.print("magnetic_field = ");
    Serial.println(magneticField);

    Serial.print("rssi_dbm = ");
    Serial.println(WiFi.RSSI());

    Serial.print("free_heap = ");
    Serial.println(ESP.getFreeHeap());

    Serial.print("ip_address = ");
    Serial.println(WiFi.localIP());

    Serial.println("================================");
}


// =====================================================
// OTA Window (เรียกเฉพาะตอนสั่ง ota_mode / กดปุ่ม BOOT)
// =====================================================

void otaWindow()
{
    bool otaModeAtStart = otaModeRequested;
    uint32_t windowMs = otaModeAtStart ? OTA_MODE_MAX_MS : OTA_WINDOW_MS;

    Serial.println();
    Serial.println("[OTA] Waiting for OTA update...");

    if (forceOTA || OTA_WAIT_INDEFINITELY)
    {
        Serial.println("[OTA] Window = unlimited");
    }
    else
    {
        Serial.print("[OTA] Window = ");
        Serial.print(windowMs / 1000);
        Serial.println(" seconds");
    }

    Serial.print("[OTA] Connect to IP = ");
    Serial.println(WiFi.localIP());

    uint32_t startMs = millis();

    while (forceOTA ||
           OTA_WAIT_INDEFINITELY ||
           millis() - startMs < windowMs)
    {
        handleOTA();

        if (mqttClient.connected())
        {
            mqttClient.loop();
        }

        // ถ้าเข้าโหมดนี้เพราะ ota_mode แล้วถูกปิดกลางทาง ให้ออกจาก window
        if (otaModeAtStart && !otaModeRequested &&
            !forceOTA && !OTA_WAIT_INDEFINITELY)
        {
            Serial.println("[OTA] ota_mode turned off, leaving OTA window");
            break;
        }

        feedWatchdog();
        delay(10);
    }

    Serial.println();
    Serial.println("[OTA] OTA window finished");
}


// =====================================================
// ปิด Wi-Fi
// =====================================================

void stopWiFi()
{
    Serial.println();
    Serial.println("[WIFI] Radio OFF");

    mqttClient.disconnect();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    delay(100);
}


// =====================================================
// Deep Sleep
// =====================================================

void enterDeepSleep()
{
    Serial.println();
    Serial.print("[POWER] Deep Sleep for ");
    Serial.print(SLEEP_TIME_US / 1000000ULL);
    Serial.println(" seconds");

    Serial.flush();

    esp_sleep_enable_timer_wakeup(SLEEP_TIME_US);
    esp_deep_sleep_start();
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    // ตรวจปุ่ม BOOT เพื่อเข้าโหมดค้างรอ OTA
    pinMode(OTA_BUTTON_PIN, INPUT_PULLUP);
    forceOTA = (digitalRead(OTA_BUTTON_PIN) == LOW);

    Serial.println();
    Serial.println("========================================");
    Serial.println("   Low-Power Wireless Sensor Node");
    Serial.println("========================================");

    if (forceOTA)
    {
        Serial.println("[OTA] Force OTA mode ON (BOOT button held)");
    }

    // ---------------- Watchdog ----------------

    setupWatchdog();
    feedWatchdog();

    // ---------------- Sensor ----------------

    Serial.println();
    Serial.println("[SENSOR] Reading sensor...");

    float rawTemperature = readTemperature();
    int rawMagneticField = readMagneticField();

    Serial.print("Temperature = ");
    Serial.print(rawTemperature, 1);
    Serial.println(" C");

    Serial.print("Magnetic Field = ");
    Serial.println(rawMagneticField);

    feedWatchdog();

    // ---------------- Edge Processing ----------------

    Serial.println();
    Serial.println("[EDGE] Processing data...");

    float temperature = processTemperature(rawTemperature);
    int magneticField = processMagneticField(rawMagneticField);

    if (isnan(temperature))
    {
        Serial.println("[ERROR] Temperature invalid");
    }
    else
    {
        Serial.print("[EDGE] Temperature = ");
        Serial.print(temperature, 1);
        Serial.println(" C");

        Serial.print("[EDGE] Magnetic Field = ");
        Serial.println(magneticField);
    }

    feedWatchdog();

    // ---------------- Wi-Fi ----------------

    if (connectWiFi())
    {
        feedWatchdog();

        Serial.println("[DEBUG] Wi-Fi connected, starting OTA...");
        setupOTA();
        Serial.println("[DEBUG] OTA setup finished");

        // ---------------- ThingsBoard ----------------
        // ต่อทุกครั้ง (แม้ค่าเซนเซอร์ผิดปกติ) เพื่อให้ได้อ่านค่า ota_mode

        if (connectThingsBoard())
        {
            if (!isnan(temperature))
            {
                showTelemetry(temperature, magneticField);
                sendTelemetry(temperature, magneticField);
            }

            // รอคำตอบจาก ThingsBoard (ota_mode) สูงสุด 4 วินาที
            uint32_t t = millis();
            while (!tbReplied && millis() - t < 4000)
            {
                mqttClient.loop();
                feedWatchdog();
                delay(10);
            }

            // เผื่อเวลาให้ข้อความที่เหลือเข้ามาอีกเล็กน้อย
            t = millis();
            while (millis() - t < 500)
            {
                mqttClient.loop();
                feedWatchdog();
                delay(10);
            }
        }
        else
        {
            Serial.println("[ERROR] ThingsBoard unavailable");
        }

        // ---------------- OTA Window ----------------
        // เปิดรอ OTA เฉพาะตอนสั่ง ota_mode / กดปุ่ม BOOT เท่านั้น

        if (otaModeRequested || forceOTA || OTA_WAIT_INDEFINITELY)
        {
            otaWindow();
        }
        else
        {
            Serial.println("[OTA] ota_mode off, skipping OTA window");
        }

        // ---------------- ปิด Wi-Fi ----------------

        stopWiFi();
    }
    else
    {
        Serial.println();
        Serial.println("[ERROR] Wi-Fi unavailable");
        Serial.println("[POWER] Skipping MQTT and OTA");
    }

    // ---------------- Deep Sleep ----------------

    feedWatchdog();
    enterDeepSleep();
}


// =====================================================
// LOOP (ไม่เคยทำงาน เพราะ setup() จบที่ Deep Sleep)
// =====================================================

void loop()
{
}