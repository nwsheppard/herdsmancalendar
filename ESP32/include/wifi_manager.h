#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * Connect to WiFi with the given credentials, retrying until timeout_ms
 * elapses. Logs progress and the result to Serial.
 * @return true if connected, false on timeout.
 */
bool wifi_connect(const String & ssid, const String & password, uint32_t timeout_ms = 20000);

/**
 * Calls WiFi.reconnect() if WiFi isn't currently connected and at least
 * reconnect_interval_ms has passed since the last reconnect attempt made
 * through this function -- meant to be called unconditionally from loop()
 * every iteration, internally rate-limited so it's a no-op most of the
 * time.
 *
 * 2026-08: this project used to also have a wifi_force_reconnect(), called
 * whenever a calendar fetch failed, on the theory that WiFi.status() could
 * keep reporting WL_CONNECTED while requests silently failed underneath
 * it. Removed -- confirmed directly, repeatedly, on hardware that forcing
 * a disconnect/reconnect never actually restored a working connection,
 * including once on the very call (/health) that had never failed before
 * in this entire investigation. A WiFi-level reconnect only resets 802.11
 * association + gets a fresh DHCP lease; it can't fix anything living
 * below that layer, which is where the real symptom (a plain TCP connect()
 * timing out waiting for a handshake) was eventually confirmed to
 * actually be -- see wifi_connect()'s own WiFi.setSleep(false) comment.
 */
void wifi_reconnect_if_down(uint32_t reconnect_interval_ms);
