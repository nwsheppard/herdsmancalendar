#include <Arduino.h>
#include <time.h>

#include "alert_manager.h"
#include "calendar_view.h"

namespace {

struct TimedEvent {
    long id;
    time_t timestamp;
    String name;
};

std::vector<TimedEvent> timed_events;

// Per-event-id "already done" tracking so a threshold staying crossed for
// several ticks (the ~1-minute refresh window) doesn't repeat the refresh
// every single tick.
std::vector<long> refreshed_ids;

struct ActiveEvent {
    long id;
    long seconds_left;
};

// Every timed event currently within countdown_window_s, recomputed each
// tick -- not just the single soonest, so simultaneous releases (several
// events at the same scheduled time) all get a countdown, not just
// whichever one the scan happens to see first.
std::vector<ActiveEvent> active_events;

uint32_t last_check_ms = 0;
constexpr uint32_t check_interval_ms = 1000;

// Real Forex Factory event IDs (data-event-id) are always positive, so
// negative, decrementing IDs for injected test events can never collide
// with a real one or with each other across repeated injections.
long next_test_event_id = -1;

constexpr long countdown_window_s = 10 * 60;
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
 * Parses CalendarEvent.day ("Fri Jul 27") + .time ("8:30am") into a real
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

    // Skip the weekday abbreviation (e.g. "Fri") up to its first space,
    // leaving "Jul 27" -- calendar_api.py's day field used to glue the
    // weekday directly onto the month with no separator ("FriJul 27", a
    // BeautifulSoup get_text() quirk, fixed server-side), but a real space
    // is more robust to parse than a hardcoded 3-char skip regardless.
    const int weekday_space = event.day.indexOf(' ');
    if (weekday_space < 0) {
        return false;
    }
    const String month_and_day = event.day.substring(weekday_space + 1);
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
            timed_events.push_back({event.id, timestamp, event.name});
        }
    }
}

void alert_manager_inject_test_event(long seconds_from_now, long reuse_event_id)
{
    struct tm now_tm;
    if (!getLocalTime(&now_tm, 0)) {
        Serial.println("Can't inject test event -- time not synced yet (NTP still in progress?)");
        return;
    }
    const time_t now = mktime(&now_tm);

    const long id = reuse_event_id != 0 ? reuse_event_id : next_test_event_id--;
    const TimedEvent test_event = {id, now + seconds_from_now, "TEST EVENT"};
    timed_events.push_back(test_event);

    Serial.printf("Injected test event: id=%ld%s, %ld seconds from now\n", test_event.id,
                  reuse_event_id != 0 ? " (reusing a real, displayed row)" : " (synthetic -- no row to highlight)",
                  seconds_from_now);
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
        active_events.clear(); // time not synced yet -- nothing to responsibly show or act on
        return;
    }
    const time_t now = mktime(&now_tm);

    active_events.clear();

    for (const TimedEvent & event : timed_events) {
        const long seconds_until = static_cast<long>(event.timestamp - now);

        if (seconds_until > 0 && seconds_until <= countdown_window_s) {
            active_events.push_back({event.id, seconds_until});
        }

        const long seconds_since = -seconds_until;
        if (seconds_since >= refresh_delay_s && seconds_since <= refresh_window_s &&
            !contains(refreshed_ids, event.id)) {
            refreshed_ids.push_back(event.id);
            calendar_view_refresh();
        }
    }
}

std::vector<long> alert_manager_get_active_event_ids()
{
    std::vector<long> ids;
    ids.reserve(active_events.size());
    for (const ActiveEvent & event : active_events) {
        ids.push_back(event.id);
    }
    return ids;
}

String alert_manager_get_countdown_text_for(long event_id)
{
    for (const ActiveEvent & event : active_events) {
        if (event.id == event_id) {
            char buffer[16];
            snprintf(buffer, sizeof(buffer), "%02ld:%02ld", event.seconds_left / 60, event.seconds_left % 60);
            return buffer;
        }
    }
    return "";
}
