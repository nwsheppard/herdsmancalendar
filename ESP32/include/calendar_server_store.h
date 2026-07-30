#pragma once

#include <Arduino.h>

/**
 * Persistent (NVS-backed) storage for the calendar API's base URL --
 * mirrors wifi_credentials_store.h. A sold product can't ship with one
 * customer's self-hosted container address (or anyone's, really) baked
 * into the firmware, so like WiFi credentials, this is entered on-device
 * (Settings screen) rather than compiled in.
 */

/** Load the stored base URL (e.g. "http://192.168.1.50:8080"). False if none saved yet. */
bool calendar_server_url_load(String & out);
/** Save the base URL, overwriting whatever was stored before. */
void calendar_server_url_save(const String & url);
