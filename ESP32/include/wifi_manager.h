#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * Connect to WiFi with the given credentials, retrying until timeout_ms
 * elapses. Logs progress and the result to Serial.
 * @return true if connected, false on timeout.
 */
bool wifi_connect(const String & ssid, const String & password, uint32_t timeout_ms = 20000);
