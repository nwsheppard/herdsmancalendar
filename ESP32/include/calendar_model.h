#pragma once

#include <Arduino.h>

/**
 * One economic calendar event, as returned by calendar_api.py's /calendar
 * endpoint. Field names/shape mirror that JSON directly (day, time,
 * currency, impact, impact_level, name, actual, forecast, previous) --
 * calendar_client_get_calendar() populates this by parsing the response,
 * and calendar_view.cpp renders it as-is.
 *
 * No "is_next" (highlight the upcoming event) field yet -- that needs a
 * real wall-clock time to compare each event's day/time against, which
 * this firmware doesn't have (no NTP sync yet). That's Milestone 8's
 * problem (countdown/alert logic), not this one's.
 */
struct CalendarEvent {
    long id = 0; // Forex Factory's own stable per-event ID -- see alert_manager.h
    String day;
    String time;
    String currency;
    String impact; // "holiday", "low", "medium", "high", or "unknown"
    int impact_level = 0; // 0 = holiday, 1 = low, 2 = medium, 3 = high
    String name;
    String actual;
    String actual_state; // "better", "worse", or "neutral" (vs forecast)
    String forecast;
    String previous;
    String previous_state; // "better", "worse", or "neutral" (vs originally-reported previous)
    bool previous_revised = false;
};
