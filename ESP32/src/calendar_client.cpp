#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "calendar_client.h"

namespace {

String base_url;

/**
 * GET helper: returns the response body, or empty string logged to Serial
 * on failure. Every call logs its own elapsed time -- this is the one
 * place all GET requests (health/filters/currencies/columns) go through,
 * so it's the cheapest place to see exactly where time goes on a "why is
 * this screen slow" report, rather than guessing (e.g. currencies/columns
 * are cached in RAM after their first fetch each power-on -- a repeat
 * "GET .../currencies" line here would mean that cache isn't working;
 * its absence means something else is the actual cost, most likely the
 * unavoidable per-open /filters round-trip or plain WiFi/HTTPClient
 * latency, not re-fetching data that's already static).
 */
String http_get(const String & path)
{
    if (base_url.length() == 0) {
        Serial.println("Calendar server not configured (Settings) -- skipping request");
        return "";
    }

    HTTPClient http;
    const String url = base_url + path;
    http.begin(url);
    http.setTimeout(8000);

    const uint32_t start_ms = millis();
    const int status = http.GET();
    const uint32_t elapsed_ms = millis() - start_ms;

    String body;
    if (status == HTTP_CODE_OK) {
        body = http.getString();
    } else {
        Serial.printf("GET %s failed, status/error: %d\n", url.c_str(), status);
    }
    Serial.printf("GET %s -> %d (%lu ms)\n", url.c_str(), status, static_cast<unsigned long>(elapsed_ms));
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
    const String body = http_get("/calendar?range=" + range);
    if (body.length() == 0) {
        return false;
    }

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
    http.setTimeout(8000);

    const int status = http.POST(payload);
    if (status != HTTP_CODE_OK) {
        Serial.printf("POST %s failed, status/error: %d, body: %s\n", url.c_str(), status,
                      http.getString().c_str());
    }
    http.end();
    return status == HTTP_CODE_OK;
}
