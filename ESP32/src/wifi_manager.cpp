#include <Arduino.h>
#include <WiFi.h>

#include "wifi_manager.h"

namespace {
// Shared by wifi_force_reconnect() and wifi_reconnect_if_down() -- see the
// latter's header comment for why this needs to be one timestamp both
// paths update, not two independent ones.
uint32_t last_reconnect_attempt_ms = 0;
} // namespace

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

void wifi_force_reconnect()
{
    Serial.println("Forcing a full WiFi disconnect/reconnect (repeated fetch failures with no WiFi-disconnect event -- see calendar_view.cpp)");
    // Plain disconnect(), not disconnect(true) -- the (bool wifioff) form
    // also powers down the radio, which would need a full WiFi.begin() (SSID
    // + password) to bring back up, not just reconnect(). A plain
    // disconnect + reconnect still forces fresh 802.11 association + DHCP,
    // which is what actually clears stale ARP/connection state -- it just
    // doesn't power-cycle the radio itself, which isn't needed for this.
    WiFi.disconnect();
    delay(100);
    WiFi.reconnect();
    last_reconnect_attempt_ms = millis();
}

void wifi_reconnect_if_down(uint32_t reconnect_interval_ms)
{
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }
    const uint32_t now = millis();
    if (now - last_reconnect_attempt_ms < reconnect_interval_ms) {
        return;
    }
    last_reconnect_attempt_ms = now;
    Serial.println("WiFi still down, retrying...");
    WiFi.reconnect();
}
