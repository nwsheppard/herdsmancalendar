"""
Herdsman Trading Terminal — Economic Calendar API

Scrapes the investing.com economic calendar widget (sslecal2.investing.com)
and re-serves it as clean JSON for the ESP32 to consume.

Endpoints:
  GET  /calendar?range=day    -> today's events, filtered per filters.json
  GET  /calendar?range=week   -> this week's events (default)
  GET  /filters                -> current impact-level/country filter selection
  POST /filters                -> update the filter selection (persisted to filters.json)
  GET  /countries              -> all countries investing.com supports, for a picker UI
  GET  /health                 -> simple liveness check

NOTE ON MAINTENANCE: this scrapes investing.com's HTML, which can change
without notice. If events stop appearing, the first thing to check is
whether the CSS selectors below (WIDGET_TABLE_ID, EVENT_ROW_SELECTOR, etc.)
still match the live page — view-source the widget URL and compare.
"""

from fastapi import FastAPI, Query, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel, field_validator
from curl_cffi import requests
from bs4 import BeautifulSoup
from datetime import datetime
from pathlib import Path
from contextlib import asynccontextmanager
import json
import logging

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("calendar_api")


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Without this, filters.json only gets created the first time something
    # calls load_filters() -- i.e. the first /calendar or /filters request,
    # not when the service actually starts. That's surprising for anyone
    # restarting/updating the service and checking for the file right after
    # (nothing to look at yet, no filters set, no obvious reason why).
    load_filters()
    yield


app = FastAPI(title="Herdsman Trading Terminal Calendar API", lifespan=lifespan)

# --- Configuration -----------------------------------------------------

BASE_URL = "https://sslecal2.investing.com/"

# importance/countries are no longer static here -- they're loaded from
# FILTERS_PATH (see load_filters()) so each deployment can configure its own
# selection via GET/POST /filters instead of a firmware/code change.
# columns/features/lang/timeZone are display prefs for investing.com's own
# rendering -- harmless to keep even though we're parsing the HTML ourselves.
#
# exc_flags/exc_currency are kept in `columns` even though the ESP32 UI
# doesn't ask for them explicitly: dropping them removes the currency-flag
# cell from the HTML entirely, which breaks the currency field below
# (verified directly -- not a guess).
DEFAULT_PARAMS = {
    "columns": "exc_flags,exc_currency,exc_importance,exc_actual,exc_forecast,exc_previous",
    "category": "_employment,_economicActivity,_inflation,_credit,_centralBanks,_Bonds",
    "features": "datepicker,timezone",
    "timeZone": "8",
    "lang": "1",
}

# --- Filters (impact level + countries), user-configurable via /filters ----

FILTERS_PATH = Path(__file__).parent / "filters.json"
DEFAULT_FILTERS = {"importance": [2, 3], "countries": [5]}

# Every country investing.com's economic calendar widget supports, scraped
# directly from investing.com's own widget customization tool
# (investing.com/webmaster-tools/economic-calendar, each country checkbox's
# `id` attribute is its code) -- not a guessed/third-party list. Re-scrape
# that page if a code is ever suspected stale; investing.com doesn't publish
# this mapping anywhere else.
COUNTRY_NAMES = {
    4: "United Kingdom", 5: "United States", 6: "Canada", 7: "Mexico",
    8: "Bermuda", 9: "Sweden", 10: "Italy", 11: "South Korea",
    12: "Switzerland", 14: "India", 15: "Costa Rica", 17: "Germany",
    20: "Nigeria", 21: "Netherlands", 22: "France", 23: "Israel",
    24: "Denmark", 25: "Australia", 26: "Spain", 27: "Chile",
    29: "Argentina", 32: "Brazil", 33: "Ireland", 34: "Belgium",
    35: "Japan", 36: "Singapore", 37: "China", 38: "Portugal",
    39: "Hong Kong", 41: "Thailand", 42: "Malaysia", 43: "New Zealand",
    44: "Pakistan", 45: "Philippines", 46: "Taiwan", 47: "Bangladesh",
    48: "Indonesia", 51: "Greece", 52: "Saudi Arabia", 53: "Poland",
    54: "Austria", 55: "Czech Republic", 56: "Russia", 57: "Kenya",
    59: "Egypt", 60: "Norway", 61: "Ukraine", 63: "Turkiye",
    66: "Iraq", 68: "Lebanon", 70: "Bulgaria", 71: "Finland",
    72: "Euro Zone", 74: "Ghana", 75: "Zimbabwe", 78: "Cote D'Ivoire",
    80: "Rwanda", 82: "Mozambique", 84: "Zambia", 85: "Tanzania",
    86: "Angola", 87: "Oman", 89: "Estonia", 90: "Slovakia",
    92: "Jordan", 93: "Hungary", 94: "Kuwait", 95: "Albania",
    96: "Lithuania", 97: "Latvia", 100: "Romania", 102: "Kazakhstan",
    103: "Luxembourg", 105: "Morocco", 106: "Iceland", 107: "Cyprus",
    109: "Malta", 110: "South Africa", 111: "Malawi", 112: "Slovenia",
    113: "Croatia", 114: "Azerbaijan", 119: "Jamaica", 121: "Ecuador",
    122: "Colombia", 123: "Uganda", 125: "Peru", 138: "Venezuela",
    139: "Mongolia", 143: "United Arab Emirates", 145: "Bahrain",
    148: "Paraguay", 162: "Sri Lanka", 163: "Botswana", 168: "Uzbekistan",
    170: "Qatar", 172: "Namibia", 174: "Bosnia-Herzegovina", 178: "Vietnam",
    180: "Uruguay", 188: "Mauritius", 193: "Palestinian Territory",
    202: "Tunisia", 204: "Kyrgyzstan", 232: "Cayman Islands", 238: "Serbia",
    247: "Montenegro",
}


class FiltersUpdate(BaseModel):
    importance: list[int]
    countries: list[int]

    @field_validator("importance")
    @classmethod
    def validate_importance(cls, value):
        if not value or any(level not in (1, 2, 3) for level in value):
            raise ValueError("importance must be a non-empty list containing only 1, 2, and/or 3")
        return value

    @field_validator("countries")
    @classmethod
    def validate_countries(cls, value):
        if not value or any(code not in COUNTRY_NAMES for code in value):
            raise ValueError("countries must be a non-empty list of codes from GET /countries")
        return value


def load_filters() -> dict:
    if not FILTERS_PATH.exists():
        save_filters(DEFAULT_FILTERS)
        return dict(DEFAULT_FILTERS)
    try:
        with FILTERS_PATH.open("r", encoding="utf-8") as f:
            data = json.load(f)
        return {"importance": data["importance"], "countries": data["countries"]}
    except (json.JSONDecodeError, KeyError, OSError) as e:
        log.warning("Failed to read %s (%s) -- falling back to defaults", FILTERS_PATH, e)
        return dict(DEFAULT_FILTERS)


def save_filters(filters: dict) -> None:
    with FILTERS_PATH.open("w", encoding="utf-8") as f:
        json.dump(filters, f)

HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
    )
}

# Selectors -- based on the widget's known table structure. If scraping
# breaks, these are the first things to re-check against the live page.
WIDGET_TABLE_ID = "ecEventsTable"
EVENT_ROW_ID_SUBSTR = "eventRowId"

IMPACT_LABELS = {1: "low", 2: "medium", 3: "high"}

# Maps the sentiment cell's title attribute (e.g. "High Volatility
# Expected") to an impact level. Primary signal for impact detection --
# semantic text, less likely to silently break than the icon class names
# below, which already have (as of 2026-07): investing.com renders them as
# lowercase "grayFullBullishIcon"/"grayEmptyBullishIcon", not the ucfirst
# "GrayFullBullish" this scraper originally assumed. Confirmed by fetching
# the live page and inspecting the actual markup, not guessed.
IMPACT_TITLE_KEYWORDS = {
    "low": 1,
    "moderate": 2,
    "high": 3,
}

# --- Scraping logic ------------------------------------------------------

def fetch_calendar_html(cal_type: str) -> str:
    filters = load_filters()
    params = dict(DEFAULT_PARAMS)
    params["importance"] = ",".join(str(level) for level in filters["importance"])
    params["countries"] = ",".join(str(code) for code in filters["countries"])
    params["calType"] = cal_type  # "day" or "week"

    # investing.com sits behind Cloudflare bot management, which blocks on
    # TLS/HTTP client fingerprint rather than headers -- a stock `requests`
    # call gets a 403 here even with a full browser header set. curl_cffi's
    # impersonate="chrome" matches Chrome's actual TLS fingerprint, which is
    # what actually gets past it.
    resp = requests.get(
        BASE_URL, params=params, headers=HEADERS, timeout=15, impersonate="chrome"
    )
    resp.raise_for_status()
    return resp.text


def parse_calendar(html: str) -> list[dict]:
    soup = BeautifulSoup(html, "html.parser")
    table = soup.find("table", {"id": WIDGET_TABLE_ID})
    if table is None:
        # Selector likely stale -- surface a clear error rather than
        # silently returning nothing.
        raise RuntimeError(
            f"Could not find table#{WIDGET_TABLE_ID} in response -- "
            "investing.com's markup may have changed. Check selectors."
        )

    rows = table.find_all("tr", id=lambda v: v and EVENT_ROW_ID_SUBSTR in v)
    events = []

    current_day = None

    for row in rows:
        classes = row.get("class") or []

        # Day separator rows typically carry the date and no event data.
        if "theDay" in classes:
            current_day = row.get_text(strip=True)
            continue

        try:
            time_cell = row.find("td", {"class": "time"})
            currency_cell = row.find("td", {"class": "flagCur"})
            impact_cell = row.find("td", {"class": "sentiment"})
            event_cell = row.find("td", {"class": "event"})
            actual_cell = row.find("td", {"class": "act"})
            forecast_cell = row.find("td", {"class": "fore"})
            previous_cell = row.find("td", {"class": "prev"})

            if event_cell is None:
                continue

            impact_level = parse_impact_level(impact_cell)
            impact_label = IMPACT_LABELS.get(impact_level, "unknown")

            # The flag <span> inside this cell is decorative (just a CSS
            # flag icon, no text) -- the currency code is a plain text node
            # in the <td> alongside it, e.g. <td class="flagCur"><span
            # class="ceFlags United_States"></span>USD</td>. Reading the
            # span's own text (as this used to) always returned "".
            currency = currency_cell.get_text(strip=True) if currency_cell else ""

            events.append({
                "day": current_day,
                "time": time_cell.get_text(strip=True) if time_cell else "",
                "currency": currency,
                "impact": impact_label,
                "impact_level": impact_level,
                "name": event_cell.get_text(strip=True),
                "actual": actual_cell.get_text(strip=True) if actual_cell else "",
                "forecast": forecast_cell.get_text(strip=True) if forecast_cell else "",
                "previous": previous_cell.get_text(strip=True) if previous_cell else "",
            })
        except Exception as e:
            # Don't let one malformed row kill the whole response.
            log.warning("Skipping malformed row: %s", e)
            continue

    return events


def parse_impact_level(impact_cell) -> int:
    """
    Impact level from the sentiment cell: title text first ("High/Moderate/Low
    Volatility Expected"), falling back to counting filled bull icons if the
    title's ever missing. See IMPACT_TITLE_KEYWORDS for why title is primary.
    """
    if impact_cell is None:
        return 0

    title = (impact_cell.get("title") or "").lower()
    for keyword, level in IMPACT_TITLE_KEYWORDS.items():
        if keyword in title:
            return level

    filled = impact_cell.find_all(
        "i", {"class": lambda c: c and "grayfullbullish" in c.lower()}
    )
    return len(filled) if filled else 0


# --- API endpoints ---------------------------------------------------------

@app.get("/health")
def health():
    return {"status": "ok", "time": datetime.utcnow().isoformat()}


@app.get("/calendar")
def get_calendar(range: str = Query("week", pattern="^(day|week)$")):
    """
    range=day  -> today's events only
    range=week -> this week's events (default)
    """
    try:
        html = fetch_calendar_html(range)
        events = parse_calendar(html)
    except requests.exceptions.RequestException as e:
        log.error("Fetch failed: %s", e)
        raise HTTPException(status_code=502, detail=f"Upstream fetch failed: {e}")
    except RuntimeError as e:
        log.error("Parse failed: %s", e)
        raise HTTPException(status_code=502, detail=str(e))

    return JSONResponse({
        "range": range,
        "fetched_at": datetime.utcnow().isoformat(),
        "count": len(events),
        "events": events,
    })


@app.get("/filters")
def get_filters():
    return load_filters()


@app.post("/filters")
def update_filters(filters: FiltersUpdate):
    data = {"importance": filters.importance, "countries": filters.countries}
    save_filters(data)
    log.info("Filters updated: %s", data)
    return data


@app.get("/countries")
def get_countries():
    return [
        {"code": code, "name": name}
        for code, name in sorted(COUNTRY_NAMES.items(), key=lambda item: item[1])
    ]


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8080)
