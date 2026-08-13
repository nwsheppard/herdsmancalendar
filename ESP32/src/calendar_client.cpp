#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "calendar_client.h"

namespace {

String base_url;

// Serializes every HTTP call this project makes (http_get() below, and
// calendar_client_save_filters()'s own POST) across every caller --
// background tasks (calendar_view.cpp's refresh_task(), main.cpp's
// wifi_keepalive_task) and direct calls from the LVGL task alike (Settings
// screens calling calendar_client_get_currencies()/get_columns() etc
// synchronously). Confirmed directly on hardware: the periodic calendar
// refresh (hourly) and the WiFi keep-alive ping (every 5 minutes) have
// intervals that divide evenly, so they're guaranteed to collide exactly
// once an hour -- two concurrent HTTPClient calls from two different
// tasks at the same instant corrupted something shared at the WiFiClient/
// lwIP layer badly enough that *every* request kept failing for a long
// stretch afterward, not just the two that collided. HTTPClient/WiFiClient
// were never designed for concurrent use from multiple tasks; this makes
// that explicit instead of relying on lucky timing to avoid it. Function-
// local static, not a namespace-scope global initialized at startup --
// C++11 guarantees thread-safe one-time initialization for this, and it
// sidesteps any question of static-initialization-order relative to
// FreeRTOS's own scheduler startup.
SemaphoreHandle_t http_mutex()
{
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

// Default for every endpoint except /calendar (see calendar_client_get_calendar()'s
// own timeout below) -- these all run entirely on calendar_api.py's own LXC,
// no upstream round-trip involved, so 8s is already generous.
//
// uint16_t, not a rounder uint32_t: matches HTTPClient::setTimeout()'s own
// parameter type below (tops out at 65535ms) -- confirmed directly on
// hardware that a wider type here just moves the overflow trap to whoever
// calls http_get() with something >65535 instead of catching it at the
// call site, which is exactly what happened once already (see
// calendar_client_get_calendar()'s own comment).
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

    xSemaphoreTake(http_mutex(), portMAX_DELAY);

    HTTPClient http;
    const String url = base_url + path;
    http.begin(url);
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

    xSemaphoreGive(http_mutex());
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
    // /calendar used to route through FlareSolverr server-side inline (see
    // calendar_api.py's old fetch_calendar_html()-per-request design),
    // needing up to 65s of client-side timeout budget to cover that -- see
    // this project's git history/README for that whole saga (a uint16_t
    // overflow, a WiFiClient/lwIP corruption bug from two concurrent
    // fetches colliding, and more). calendar_api.py was restructured
    // (2026-08) so a background thread refreshes its cache on its own
    // schedule and /calendar always just reads from it -- a plain local
    // dict lookup now, same cost profile as every other endpoint here, so
    // this uses the plain default_timeout_ms like all of them rather than
    // a special extended one. A fetch that actually needs longer than that
    // now means something is genuinely stuck, not FlareSolverr being slow.
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

    xSemaphoreTake(http_mutex(), portMAX_DELAY);

    HTTPClient http;
    const String url = base_url + "/filters";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);
    http.setReuse(false); // see http_get()'s own comment on this

    const int status = http.POST(payload);
    if (status != HTTP_CODE_OK) {
        Serial.printf("POST %s failed, status/error: %d, body: %s\n", url.c_str(), status,
                      http.getString().c_str());
    }
    http.end();

    xSemaphoreGive(http_mutex());
    return status == HTTP_CODE_OK;
}
