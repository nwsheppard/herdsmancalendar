#include <Arduino.h>
#include <WiFi.h>

#include "wifi_manager.h"

bool wifi_connect(const String & ssid, const String & password, uint32_t timeout_ms)
{
    Serial.printf("Connecting to WiFi SSID '%s'...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeout_ms) {
            Serial.println("WiFi connect timed out");
            return false;
        }
        delay(250);
        Serial.print(".");
    }
    Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}
