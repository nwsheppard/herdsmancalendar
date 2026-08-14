#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "calendar_client.h"

namespace {

String base_url;

// Every endpoint here runs entirely on calendar_api.py's own LXC (its own
// background thread handles the actual Forex Factory fetch -- see its own
// module docstring), so 8s is generous for all of them, /calendar
// included.
//
// uint16_t, not a rounder uint32_t: matches HTTPClient::setTimeout()'s own
// parameter type below (tops out at 65535ms) -- confirmed directly on
// hardware that a wider type here just moves the overflow trap to whoever
// calls http_get() with something >65535 instead of catching it at the
// call site, which is exactly what happened once already (see this
// project's git history/README for that saga).
constexpr uint16_t default_timeout_ms = 8000;

/**
 * GET helper: returns the response body, or empty string logged to Serial
 * on failure. Every call logs its own elapsed time -- this is the one
 * place all GET requests (health/filters/currencies/columns/calendar) go
 * through, so it's the cheapest place to see exactly where time goes on a
 * "why is this screen slow" report, rather than guessing (e.g. currencies/
 * columns are cached in RAM after their first fetch each power-on -- a
 * repeat "GET .../currencies" line here would mean that cache isn't
 * working; its absence means something else is the actual cost, most
 * likely the unavoidable per-open /filters round-trip or plain WiFi/
 * HTTPClient latency, not re-fetching data that's already static).
 */
String http_get(const String & path, uint16_t timeout_ms = default_timeout_ms)
{
    if (base_url.length() == 0) {
        Serial.println("Calendar server not configured (Settings) -- skipping request");
        return "";
    }

    HTTPClient http;
    const String url = base_url + path;
    http.begin(url);
    // setTimeout() alone was never actually covering the connect phase --
    // confirmed directly by reading HTTPClient.cpp itself, not guessed.
    // setTimeout() only sets _tcpTimeout (the *read* timeout, applied via
    // _client->setTimeout() after a connection already exists) and even
    // that line is skipped entirely if called before connect() (which
    // every call here does, right after begin()). The actual connect()
    // call uses a completely separate _connectTimeout, defaulting to
    // HTTPCLIENT_DEFAULT_TCP_TIMEOUT (a hardcoded 5000ms) unless
    // setConnectTimeout() is called explicitly -- which this project never
    // did, across the entire investigation into intermittent connect
    // failures. Every single "-1 connection refused" logged throughout
    // that investigation timed out at ~5000-5033ms -- that hardcoded
    // default, not anything this project thought it was controlling via
    // setTimeout()'s timeout_ms argument. Setting both here closes that
    // gap: the connect phase now gets the same real budget the read phase
    // does, instead of being silently capped at 5s regardless of what
    // timeout_ms this function was actually called with.
    http.setConnectTimeout(timeout_ms);
    http.setTimeout(timeout_ms);
    // HTTPClient's own disconnect() leaves the underlying socket "open for
    // reuse" (doesn't call _client->stop()) once a response's headers have
    // started arriving, even if the request then times out before the body
    // finishes -- exactly what a read-timeout (-11) is. Every call here
    // constructs a brand-new HTTPClient, so there's no actual keep-alive
    // reuse happening across our own requests to begin with -- this costs
    // nothing and removes one possible source of a socket left in a
    // half-torn-down state after a timeout, confirmed directly on hardware
    // that a single -11 has been enough to break every request after it.
    http.setReuse(false);

    const uint32_t start_ms = millis();
    const int status = http.GET();
    const uint32_t elapsed_ms = millis() - start_ms;

    String body;
    if (status == HTTP_CODE_OK) {
        body = http.getString();
    } else {
        Serial.printf("GET %s failed, status/error: %d\n", url.c_str(), status);
    }
    // Absolute timestamp (not just elapsed) alongside the duration -- lets a
    // stall/glitch reported against wall-clock/uptime be checked against
    // this log after the fact, not just "this particular GET was slow."
    Serial.printf("GET %s -> %d (%lu ms, ending t=%lums)\n", url.c_str(), status,
                  static_cast<unsigned long>(elapsed_ms), static_cast<unsigned long>(millis()));
    http.end();
    return body;
}

/** Shared by calendar_client_get_currencies()/get_columns() -- both endpoints return the same {code, name} shape. */
bool fetch_string_options(const String & path, std::vector<StringOption> & out)
{
    const String body = http_get(path);
    if (body.length() == 0) {
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body);
    if (error) {
        Serial.printf("Failed to parse %s response: %s\n", path.c_str(), error.c_str());
        return false;
    }

    std::vector<StringOption> result;
    for (JsonObject entry : doc.as<JsonArray>()) {
        StringOption option;
        option.code = entry["code"].as<const char *>();
        option.name = entry["name"].as<const char *>();
        result.push_back(option);
    }

    out = result;
    return true;
}

} // namespace

void calendar_client_set_base_url(const String & url)
{
    base_url = url;
}

String calendar_client_check_health()
{
    const String body = http_get("/health");
    if (body.length() > 0) {
        Serial.printf("GET /health -> %s\n", body.c_str());
    }
    return body;
}

bool calendar_client_get_filters(CalendarFilters & out)
{
    const String body = http_get("/filters");
    if (body.length() == 0) {
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body);
    if (error) {
        Serial.printf("Failed to parse /filters response: %s\n", error.c_str());
        return false;
    }

    CalendarFilters result;
    result.impact_holiday = false;
    result.impact_low = false;
    result.impact_medium = false;
    result.impact_high = false;
    for (JsonVariant level : doc["importance"].as<JsonArray>()) {
        const int value = level.as<int>();
        if (value == 0) result.impact_holiday = true;
        if (value == 1) result.impact_low = true;
        if (value == 2) result.impact_medium = true;
        if (value == 3) result.impact_high = true;
    }
    for (JsonVariant code : doc["currencies"].as<JsonArray>()) {
        result.currency_codes.push_back(String(code.as<const char *>()));
    }
    for (JsonVariant code : doc["columns"].as<JsonArray>()) {
        result.column_codes.push_back(String(code.as<const char *>()));
    }

    out = result;
    return true;
}

bool calendar_client_get_currencies(std::vector<StringOption> & out)
{
    return fetch_string_options("/currencies", out);
}

bool calendar_client_get_columns(std::vector<StringOption> & out)
{
    return fetch_string_options("/columns", out);
}

bool calendar_client_get_calendar(const String & range, std::vector<CalendarEvent> & out)
{
    // calendar_api.py answers this from its own background-refreshed
    // cache -- a plain local read, always fast -- so this uses the same
    // default_timeout_ms every other endpoint here does. (This project
    // briefly used /calendar/wait, a long-polling variant, instead of
    // plain polling; reverted -- see calendar_view.cpp's poll_interval_ms
    // for why. It also briefly used a temporary 60000ms diagnostic
    // timeout here specifically -- see the README's own writeup -- which
    // confirmed a stuck read genuinely never got a response even given a
    // full 60s, not just late; that question answered, reverted back to
    // the plain default.)
    const String body = http_get("/calendar?range=" + range);
    if (body.length() == 0) {
        return false;
    }

    // Parsing (deserializeJson() plus the loop below building each
    // CalendarEvent's String fields) has no natural yield point the way
    // calendar_view.cpp's row-building loop does -- for a week's worth of
    // events (100+), this runs uninterrupted with no lv_timer_handler()
    // call in between, same hazard as the (already fixed) row-building
    // loop. Logged unconditionally, not just above some threshold -- this
    // only runs once per refresh, not once per frame, so it's cheap either
    // way and worth seeing on every call while tracking down the
    // still-recurring screen glitch.
    const uint32_t parse_start_ms = millis();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, body);
    if (error) {
        Serial.printf("Failed to parse /calendar response: %s\n", error.c_str());
        return false;
    }

    std::vector<CalendarEvent> result;
    for (JsonObject entry : doc["events"].as<JsonArray>()) {
        CalendarEvent event;
        event.id = entry["id"].as<long>();
        // day is JSON null for any event before the first day-separator row
        // is seen server-side -- shouldn't happen in practice (every
        // response starts with one), but as<const char*>() on a null
        // JsonVariant returns nullptr, which String's constructor doesn't
        // handle gracefully. Guard it explicitly rather than assume.
        event.day = entry["day"].isNull() ? String("") : String(entry["day"].as<const char *>());
        event.time = entry["time"].as<const char *>();
        event.currency = entry["currency"].as<const char *>();
        event.impact = entry["impact"].as<const char *>();
        event.impact_level = entry["impact_level"].as<int>();
        event.name = entry["name"].as<const char *>();
        event.actual = entry["actual"].as<const char *>();
        event.actual_state = entry["actual_state"].as<const char *>();
        event.forecast = entry["forecast"].as<const char *>();
        event.previous = entry["previous"].as<const char *>();
        event.previous_state = entry["previous_state"].as<const char *>();
        event.previous_revised = entry["previous_revised"].as<bool>();
        result.push_back(event);
    }

    Serial.printf("Parsed /calendar body: %u events in %lums, ending t=%lums\n",
                  static_cast<unsigned>(result.size()),
                  static_cast<unsigned long>(millis() - parse_start_ms), static_cast<unsigned long>(millis()));

    out = result;
    return true;
}

bool calendar_client_save_filters(const CalendarFilters & filters)
{
    if (base_url.length() == 0) {
        Serial.println("Calendar server not configured (Settings) -- cannot save filters");
        return false;
    }

    JsonDocument doc;
    JsonArray importance = doc["importance"].to<JsonArray>();
    if (filters.impact_holiday) importance.add(0);
    if (filters.impact_low) importance.add(1);
    if (filters.impact_medium) importance.add(2);
    if (filters.impact_high) importance.add(3);

    JsonArray currencies = doc["currencies"].to<JsonArray>();
    for (const String & code : filters.currency_codes) {
        currencies.add(code);
    }

    JsonArray columns = doc["columns"].to<JsonArray>();
    for (const String & code : filters.column_codes) {
        columns.add(code);
    }

    String payload;
    serializeJson(doc, payload);

    HTTPClient http;
    const String url = base_url + "/filters";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(8000); // see http_get()'s own comment on why this is needed alongside setTimeout()
    http.setTimeout(8000);
    http.setReuse(false); // see http_get()'s own comment on this

    const int status = http.POST(payload);
    if (status != HTTP_CODE_OK) {
        Serial.printf("POST %s failed, status/error: %d, body: %s\n", url.c_str(), status,
                      http.getString().c_str());
    }
    http.end();
    return status == HTTP_CODE_OK;
}
