#pragma once

#include <Arduino.h>

/**
 * Persistent (NVS-backed) storage for WiFi credentials, so a customer's
 * network survives reboots and firmware updates without being compiled
 * into the build. See also calendar_server_store.h, which does the same
 * for the calendar API's URL.
 */

/** Load stored credentials. Returns false (with both strings empty) if none saved. */
bool wifi_credentials_load(String & ssid, String & password);
/** Save credentials, overwriting whatever was stored before. */
void wifi_credentials_save(const String & ssid, const String & password);
/** Erase stored credentials (e.g. after a persistent connect failure). */
void wifi_credentials_clear();
