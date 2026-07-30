#pragma once

#include <Arduino.h>
#include <vector>

#include "calendar_model.h"

/**
 * Tracks upcoming timed events from the most recent fetch and drives three
 * time-based behaviors against them (herdsman-trading-terminal-brief.md's
 * "Countdown / Alert Logic" section):
 *   - a countdown display starting 10 minutes before the soonest event
 *   - a sound alert 5 minutes before, for high-impact ("red folder") events only
 *   - an automatic calendar refresh 30 seconds after an event's scheduled
 *     time, to pick up the actual value once it's been released
 *
 * Requires NTP time sync (main.cpp calls configTzTime() once after WiFi
 * connects) -- everything here is a no-op until getLocalTime() succeeds,
 * rather than acting on an unsynced clock (which would otherwise read as
 * Jan 1 1970 and misfire immediately). Event day/time strings are Forex
 * Factory's own America/New_York wall-clock text (confirmed directly: an
 * anonymous request's page declares `timezone_name: 'America/New_York'`,
 * and the page's displayed clock was exactly 4 hours behind real UTC at
 * the time of checking, matching EDT) -- main.cpp's configTzTime() call
 * sets the system's local timezone to the same zone (with its DST rules)
 * specifically so this module's mktime() calls land on the correct UTC
 * instant without hand-rolling DST math.
 */

/** Called whenever calendar_view.cpp successfully fetches a fresh event list. */
void alert_manager_set_events(const std::vector<CalendarEvent> & events);

/**
 * Call every loop() iteration. Cheap to call often -- internally rate-limits
 * its own real work to once a second, and only takes any of the three
 * actions above once per event (tracked by CalendarEvent.id), not
 * repeatedly on every tick while a threshold stays crossed.
 */
void alert_manager_tick();

/**
 * Countdown text for the calendar view to display (e.g. "NEXT: NFP in
 * 09:47"), or an empty string if no timed event is within the 10-minute
 * countdown window (including if time isn't synced yet).
 */
String alert_manager_get_countdown_text();

/**
 * Dev/test aid: injects a synthetic timed event `seconds_from_now` seconds
 * out (named "TEST EVENT", with the given impact_level) into the tracked
 * list, so the countdown/sound/refresh pipeline can be exercised on real
 * hardware without waiting for an actual calendar event to approach.
 * Triggered from main.cpp's serial command reader -- type "testalert" (or
 * "testalert <seconds>", or "testalert <seconds> <impact_level>") in the
 * serial monitor. Appended alongside whatever real events are already
 * tracked, not a replacement for them. Requires NTP sync, same as
 * everything else here; logs a message to Serial and does nothing if time
 * isn't synced yet.
 */
void alert_manager_inject_test_event(long seconds_from_now, int impact_level);
