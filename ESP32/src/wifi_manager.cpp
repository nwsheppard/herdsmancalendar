#include <Arduino.h>
#include <WiFi.h>

#include "wifi_manager.h"

namespace {
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

    // Never tried until now, despite months of chasing intermittent
    // connect()/read timeouts on this exact device: ESP32 Arduino leaves
    // WiFi modem-sleep power-save mode on by default in station mode,
    // which is a well-known, common source of exactly this symptom --
    // the radio periodically dozes and has to wake to actually send/
    // receive, adding real, sometimes multi-second delays to individual
    // packets. This terminal is mains-powered sitting next to a display
    // that's itself never sleeping -- there's no power budget here that
    // benefits from trading latency for it. Confirmed via CORE_DEBUG_LEVEL
    // diagnostics that the actual failure signature (NetworkClient.cpp's
    // own connect() log) was a plain select() timeout waiting for a TCP
    // handshake to complete on a request to a device on the same LAN, no
    // DNS involved -- a radio-level delivery symptom, not anything in this
    // project's own application code, which is exactly modem-sleep's
    // known failure mode. Confirmed directly that this alone didn't
    // resolve it, though -- reported directly: a request that succeeds
    // (connects, gets a response) can be immediately followed by another,
    // to the same host, from the same task, that fails the identical way
    // (select() timing out waiting for the handshake). That rules out
    // anything tied to a task's first use of the network, or to modem-sleep
    // wake latency specifically -- it's intermittent at the packet level,
    // not deterministic per-task or per-call.
    //
    // Also never tried: forcing maximum TX power. RSSI logged across this
    // whole investigation has ranged -32 to -53 -- not desperately weak,
    // but not strong enough to rule out marginal signal margin as a
    // contributor to intermittent packet loss, especially on a crowded
    // 2.4GHz band. Arduino-ESP32 doesn't reliably report what the default
    // actually is across versions/regions, so this sets it explicitly to
    // the maximum this API exposes rather than assuming the default is
    // already there.
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    return true;
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
