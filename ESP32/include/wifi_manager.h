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
 * Forces a full WiFi disconnect + re-associate + fresh DHCP cycle, using
 * whatever SSID/password wifi_connect() was last called with -- does not
 * need them passed again. For a specific, confirmed-on-hardware failure
 * mode: WiFi.status() keeps reporting WL_CONNECTED while every actual
 * connection attempt to the calendar server silently fails (request sent,
 * no response ever arrives, no error either), persisting until the device
 * is rebooted. That combination points at stale low-level network state
 * (most likely an ARP cache entry) that a plain reconnect-while-
 * disconnected loop (loop()'s own WiFi.reconnect() backstop) never
 * triggers, since WiFi itself never reports being disconnected in the
 * first place. A fresh association + DHCP lease is the same reset a
 * reboot gives the network stack, without needing one.
 */
void wifi_force_reconnect();

/**
 * Calls WiFi.reconnect() if WiFi isn't currently connected and at least
 * reconnect_interval_ms has passed since the last reconnect attempt made
 * through either this function or wifi_force_reconnect() -- meant to be
 * called unconditionally from loop() every iteration, internally
 * rate-limited so it's a no-op most of the time.
 *
 * Sharing one cooldown across both reconnect paths (rather than loop()
 * tracking its own separately) is what actually matters here: confirmed
 * directly on hardware that without it, wifi_force_reconnect()'s own
 * WiFi.disconnect() call made WiFi.status() report not-connected on the
 * very next loop() iteration, which loop()'s previously-independent
 * watchdog (its own separate timer, unaware a reconnect had just started)
 * treated as "WiFi's been down for a while, try again" and immediately
 * called WiFi.reconnect() a second time -- racing the first attempt that
 * was still associating and logging ESP-IDF's own "sta is connecting,
 * return error". The fetches right after that collision kept failing
 * with connection-refused, consistent with the reconnect never actually
 * completing because of it.
 */
void wifi_reconnect_if_down(uint32_t reconnect_interval_ms);
