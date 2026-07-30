#include <Arduino.h>
#include <time.h>

#include "alert_manager.h"
#include "audio_player.h"
#include "calendar_view.h"

namespace {

struct TimedEvent {
    long id;
    time_t timestamp;
    int impact_level;
    String name;
};

std::vector<TimedEvent> timed_events;

// Per-event-id "already done" tracking so a threshold staying crossed for
// several ticks (the 5-minute alert window, or even the ~1-minute refresh
// window) doesn't repeat the sound/refresh every single tick.
std::vector<long> alerted_ids;
std::vector<long> refreshed_ids;

String countdown_text;
uint32_t last_check_ms = 0;
constexpr uint32_t check_interval_ms = 1000;

// Real Forex Factory event IDs (data-event-id) are always positive, so
// negative, decrementing IDs for injected test events can never collide
// with a real one or with each other across repeated injections.
long next_test_event_id = -1;

constexpr long countdown_window_s = 10 * 60;
constexpr long alert_window_s = 5 * 60;
constexpr long refresh_delay_s = 30;
// Upper bound on the refresh window: a tick lands close to once a second,
// so without some slack a single check exactly at +30s could be missed
// between ticks. 40s gives an ample margin while still being clearly
// "shortly after," not "sometime later."
constexpr long refresh_window_s = 40;

bool contains(const std::vector<long> & ids, long id)
{
    for (long existing : ids) {
        if (existing == id) {
            return true;
        }
    }
    return false;
}

/**
 * Parses CalendarEvent.day ("MonJul 27") + .time ("8:30am") into a real
 * timestamp. Returns false for events with no fixed time ("All Day",
 * "Tentative") -- there's nothing to count down to -- or if NTP hasn't
 * synced yet (the current year is needed to disambiguate the day text,
 * which carries no year of its own).
 */
bool parse_event_timestamp(const CalendarEvent & event, time_t & out)
{
    static const char * const month_abbrevs[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    // Skip the 3-char weekday abbreviation glued to the front (e.g. "Mon"),
    // leaving "Jul 27".
    if (event.day.length() < 4) {
        return false;
    }
    const String month_and_day = event.day.substring(3);
    const int space_index = month_and_day.indexOf(' ');
    if (space_index < 0) {
        return false;
    }

    const String month_str = month_and_day.substring(0, space_index);
    const int day_num = month_and_day.substring(space_index + 1).toInt();
    if (day_num <= 0) {
        return false;
    }

    int month_index = -1;
    for (int i = 0; i < 12; ++i) {
        if (month_str.equalsIgnoreCase(month_abbrevs[i])) {
            month_index = i;
            break;
        }
    }
    if (month_index < 0) {
        return false;
    }

    // "8:30am"/"11:05pm" -- "All Day"/"Tentative"/"" have no fixed time.
    String time_str = event.time;
    time_str.toLowerCase();
    const bool is_am = time_str.endsWith("am");
    const bool is_pm = time_str.endsWith("pm");
    if (!is_am && !is_pm) {
        return false;
    }

    const String time_digits = time_str.substring(0, time_str.length() - 2);
    const int colon_index = time_digits.indexOf(':');
    if (colon_index < 0) {
        return false;
    }

    int hour = time_digits.substring(0, colon_index).toInt();
    const int minute = time_digits.substring(colon_index + 1).toInt();
    if (hour < 1 || hour > 12 || minute < 0 || minute > 59) {
        return false;
    }
    if (is_pm && hour != 12) {
        hour += 12;
    }
    if (is_am && hour == 12) {
        hour = 0;
    }

    // ms=0: return immediately based on current sync state rather than
    // blocking up to the default 5s -- this runs from a once-a-second tick,
    // not a one-off startup check.
    struct tm now_tm;
    if (!getLocalTime(&now_tm, 0)) {
        return false;
    }

    int year = now_tm.tm_year + 1900;
    // The only case a day/week-range fetch can straddle a year boundary:
    // today is December and the event's month is January, so it must be
    // next year, not this one.
    if (now_tm.tm_mon == 11 && month_index == 0) {
        year += 1;
    }

    struct tm event_tm = {};
    event_tm.tm_year = year - 1900;
    event_tm.tm_mon = month_index;
    event_tm.tm_mday = day_num;
    event_tm.tm_hour = hour;
    event_tm.tm_min = minute;
    event_tm.tm_sec = 0;
    event_tm.tm_isdst = -1; // let mktime() resolve DST via the configured TZ rules

    out = mktime(&event_tm);
    return true;
}

} // namespace

void alert_manager_set_events(const std::vector<CalendarEvent> & events)
{
    timed_events.clear();
    for (const CalendarEvent & event : events) {
        time_t timestamp;
        if (parse_event_timestamp(event, timestamp)) {
            timed_events.push_back({event.id, timestamp, event.impact_level, event.name});
        }
    }
}

void alert_manager_inject_test_event(long seconds_from_now, int impact_level)
{
    struct tm now_tm;
    if (!getLocalTime(&now_tm, 0)) {
        Serial.println("Can't inject test event -- time not synced yet (NTP still in progress?)");
        return;
    }
    const time_t now = mktime(&now_tm);

    const TimedEvent test_event = {next_test_event_id--, now + seconds_from_now, impact_level, "TEST EVENT"};
    timed_events.push_back(test_event);

    Serial.printf("Injected test event: id=%ld, %ld seconds from now, impact_level=%d\n",
                  test_event.id, seconds_from_now, impact_level);
}

void alert_manager_tick()
{
    const uint32_t now_ms = millis();
    if (now_ms - last_check_ms < check_interval_ms) {
        return;
    }
    last_check_ms = now_ms;

    struct tm now_tm;
    if (!getLocalTime(&now_tm, 0)) {
        countdown_text = ""; // time not synced yet -- nothing to responsibly show or act on
        return;
    }
    const time_t now = mktime(&now_tm);

    bool have_soonest = false;
    long soonest_seconds_left = 0;
    String soonest_name;

    for (const TimedEvent & event : timed_events) {
        const long seconds_until = static_cast<long>(event.timestamp - now);

        if (seconds_until > 0 && seconds_until <= countdown_window_s &&
            (!have_soonest || seconds_until < soonest_seconds_left)) {
            have_soonest = true;
            soonest_seconds_left = seconds_until;
            soonest_name = event.name;
        }

        if (seconds_until > 0 && seconds_until <= alert_window_s && event.impact_level == 3 &&
            !contains(alerted_ids, event.id)) {
            alerted_ids.push_back(event.id);
            audio_player_play_alert();
        }

        const long seconds_since = -seconds_until;
        if (seconds_since >= refresh_delay_s && seconds_since <= refresh_window_s &&
            !contains(refreshed_ids, event.id)) {
            refreshed_ids.push_back(event.id);
            calendar_view_refresh();
        }
    }

    if (have_soonest) {
        char buffer[96];
        snprintf(buffer, sizeof(buffer), "NEXT: %s in %02ld:%02ld", soonest_name.c_str(),
                 soonest_seconds_left / 60, soonest_seconds_left % 60);
        countdown_text = buffer;
    } else {
        countdown_text = "";
    }
}

String alert_manager_get_countdown_text()
{
    return countdown_text;
}
