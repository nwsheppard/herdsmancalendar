#pragma once

#include <Arduino.h>
#include <vector>

#include "calendar_model.h"

/**
 * Base URL used by every function below (e.g. "http://192.168.1.50:8080",
 * no trailing slash). Set once at boot from calendar_server_store.h, and
 * again whenever the Settings screen saves a new one -- there's no
 * compiled-in default, since a sold product can't assume everyone's
 * calendar_api.py container lives at the same address.
 */
void calendar_client_set_base_url(const String & url);

/**
 * GET <base url>/health and return the raw response body. Logs the
 * request/response (or failure) to Serial. Returns an empty string on
 * failure, including if no base URL has been set yet.
 */
String calendar_client_check_health();

/** One entry from GET /currencies or /columns -- both use string codes. */
struct StringOption {
    String code;
    String name;
};

/**
 * Current filter selection, as stored server-side by calendar_api.py's
 * /filters. impact_holiday corresponds to Forex Factory's "grey" impact
 * level (holidays/non-economic notices) -- off by default, since it's
 * exactly the kind of low-value noise this filter exists to hide.
 */
struct CalendarFilters {
    bool impact_holiday = false;
    bool impact_low = false;
    bool impact_medium = true;
    bool impact_high = true;
    std::vector<String> currency_codes;
    std::vector<String> column_codes;
};

/** GET /filters. Returns false (leaving `out` unchanged) on failure. */
bool calendar_client_get_filters(CalendarFilters & out);

/**
 * GET /currencies -- every currency Forex Factory's calendar covers, for
 * building a picker UI. Returns false (leaving `out` unchanged) on failure.
 */
bool calendar_client_get_currencies(std::vector<StringOption> & out);

/**
 * GET /columns -- every optional per-event data field the ESP32 can choose
 * to show (Impact/Actual/Forecast/Previous). Returns false (leaving `out`
 * unchanged) on failure.
 */
bool calendar_client_get_columns(std::vector<StringOption> & out);

/**
 * POST /filters with the given selection. Returns false on failure
 * (network error, or the server rejected it -- e.g. an empty selection).
 */
bool calendar_client_save_filters(const CalendarFilters & filters);

/**
 * GET /calendar?range=day or range=week, parsed into a chronological list
 * of events (filtered/blanked per filters.json server-side already --
 * nothing further to apply here). Returns false (leaving `out` unchanged)
 * on failure: network error, non-200 status, or malformed JSON.
 */
bool calendar_client_get_calendar(const String & range, std::vector<CalendarEvent> & out);
