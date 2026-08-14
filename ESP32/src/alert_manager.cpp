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

// Per-event-id progress through refresh_checkpoints_s (below) -- tracks
// how many of those checkpoints have already fired for a given event, so
// a threshold staying crossed for several ticks doesn't repeat the same
// refresh every single tick.
struct EventRefreshProgress {
    long id;
    size_t next_checkpoint = 0;
};
std::vector<EventRefreshProgress> refresh_progress;

EventRefreshProgress & progress_for(long id)
{
    for (EventRefreshProgress & progress : refresh_progress) {
        if (progress.id == id) {
            return progress;
        }
    }
    refresh_progress.push_back({id, 0});
    return refresh_progress.back();
}

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

// Checkpoints (seconds after an event's own scheduled time) to refresh at
// -- not just one. Reported directly from hardware: an 8:30 event's actual
// value hadn't landed on screen even after the original single 30-40s
// post-event refresh. Root cause: calendar_api.py's own background cache
// refreshes near an event on its own clock (every
// CALENDAR_REFRESH_INTERVAL_SHORT_S, 5 minutes by default -- see its own
// module docstring), not synchronized to any specific event's exact
// scheduled time. A single fixed-window check can land in the gap right
// after the backend's last refresh and before its next one, missing a
// value that would have shown up if anyone had asked again a few minutes
// later. Multiple checkpoints, spaced further apart each time, give that
// gap room to close without polling continuously forever: close together
// early (a value landing right around its release time is the common
// case), further apart later (chasing a genuinely delayed report, not
// wasting fetches indefinitely).
constexpr long refresh_checkpoints_s[] = {30, 90, 210, 390};
constexpr size_t refresh_checkpoint_count = sizeof(refresh_checkpoints_s) / sizeof(refresh_checkpoints_s[0]);
// Tolerance around each checkpoint -- a tick lands close to once a second,
// not exactly, so without some slack a checkpoint could be missed between
// ticks.
constexpr long refresh_checkpoint_window_s = 15;

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
        if (seconds_since < 0) {
            continue;
        }

        EventRefreshProgress & progress = progress_for(event.id);
        if (progress.next_checkpoint >= refresh_checkpoint_count) {
            continue; // already worked through every checkpoint for this event
        }

        const long checkpoint = refresh_checkpoints_s[progress.next_checkpoint];
        if (seconds_since >= checkpoint && seconds_since <= checkpoint + refresh_checkpoint_window_s) {
            ++progress.next_checkpoint;
            calendar_view_refresh();
        } else if (seconds_since > checkpoint + refresh_checkpoint_window_s) {
            // Missed this checkpoint's own window entirely (a tick gap,
            // e.g. right after boot for an event that was already past
            // this checkpoint) -- move on to the next one's own timing
            // rather than firing a late catch-up refresh outside the
            // window this checkpoint was actually meant to represent.
            ++progress.next_checkpoint;
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
