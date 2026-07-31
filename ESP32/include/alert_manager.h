#pragma once

#include <Arduino.h>
#include <vector>

#include "calendar_model.h"

/**
 * Tracks upcoming timed events from the most recent fetch and drives two
 * time-based behaviors against them (herdsman-trading-terminal-brief.md's
 * "Countdown / Alert Logic" section):
 *   - a countdown display starting 10 minutes before the soonest event
 *   - an automatic calendar refresh 30 seconds after an event's scheduled
 *     time, to pick up the actual value once it's been released
 *
 * A sound alert (5 minutes before, for high-impact events) was originally
 * part of this too, but was dropped: playing a tone over I2S reliably
 * corrupted the RGB display on real hardware (confirmed directly -- heavy
 * pixelation and, separately, a "frame shift" with content wrapping toward
 * the bottom of the screen), and neither a larger LovyanGFX bounce buffer
 * nor a much lower I2S sample rate resolved it. The documented fix
 * (`CONFIG_LCD_RGB_RESTART_IN_VSYNC`) needs ESP-IDF Kconfig access, which
 * this project's `framework = arduino` (prebuilt libraries) build mode
 * doesn't have -- reaching it means switching to the combined
 * `arduino, espidf` build, which was attempted and hit a string of
 * unrelated build-system issues (see git history on the
 * `esp32-espidf-build-mode` branch) deep enough to abandon for now rather
 * than keep absorbing risk for a nice-to-have. `audio_player.*` was
 * removed along with the sound alert.
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
 * its own real work to once a second, and only triggers the refresh once
 * per event (tracked by CalendarEvent.id), not repeatedly on every tick
 * while its threshold stays crossed.
 */
void alert_manager_tick();

/**
 * CalendarEvent.id of every timed event currently within the 10-minute
 * countdown window (empty if none, including if time isn't synced yet).
 * Every one of these gets its own live countdown, not just the single
 * soonest -- simultaneous releases (several indicators dropping at the
 * same scheduled time, common on a busy morning) are routine enough for a
 * trading calendar that only ever highlighting one of them would be
 * misleading, not just an edge case.
 */
std::vector<long> alert_manager_get_active_event_ids();

/**
 * Countdown text as "MM:SS" (e.g. "09:47") for a specific event id, or an
 * empty string if that id isn't currently within the countdown window (or
 * isn't tracked at all). Just the time -- the calendar view shows this
 * directly on the relevant event's own row (see
 * alert_manager_get_active_event_ids()), so the event's name doesn't need
 * repeating here.
 */
String alert_manager_get_countdown_text_for(long event_id);

/**
 * Dev/test aid: injects a synthetic timed event `seconds_from_now` seconds
 * out, so the countdown/refresh pipeline can be exercised on real hardware
 * without waiting for an actual calendar event to approach. Triggered from
 * main.cpp's serial command reader -- type "testalert" (or "testalert
 * <seconds>") in the serial monitor. Appended alongside whatever real
 * events are already tracked, not a replacement for them. Requires NTP
 * sync, same as everything else here; logs a message to Serial and does
 * nothing if time isn't synced yet.
 *
 * `reuse_event_id`, if non-zero, is a real CalendarEvent.id (see
 * calendar_view_get_first_event_id()) to attach the test countdown to
 * instead of a synthetic id -- the countdown lives directly on its event's
 * row now (see calendar_view.cpp's update_next_event_row()), so without a
 * matching real, currently-displayed row there's nothing on screen for it
 * to highlight. Falls back to a synthetic decrementing id (named "TEST
 * EVENT") if 0 or omitted, which still exercises alert_manager's own
 * tracking/refresh logic but won't visibly highlight any row.
 */
void alert_manager_inject_test_event(long seconds_from_now, long reuse_event_id = 0);
