"""
Herdsman Trading Terminal — Economic Calendar API

Scrapes the investing.com economic calendar widget (sslecal2.investing.com)
and re-serves it as clean JSON for the ESP32 to consume.

Endpoints:
  GET /calendar?range=day     -> today's events
  GET /calendar?range=week    -> this week's events (default)
  GET /health                 -> simple liveness check

NOTE ON MAINTENANCE: this scrapes investing.com's HTML, which can change
without notice. If events stop appearing, the first thing to check is
whether the CSS selectors below (WIDGET_TABLE_ID, EVENT_ROW_SELECTOR, etc.)
still match the live page — view-source the widget URL and compare.
"""

from fastapi import FastAPI, Query, HTTPException
from fastapi.responses import JSONResponse
import requests
from bs4 import BeautifulSoup
from datetime import datetime
import logging

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("calendar_api")

app = FastAPI(title="Herdsman Trading Terminal Calendar API")

# --- Configuration -----------------------------------------------------

BASE_URL = "https://sslecal2.investing.com/"

# These mirror the params from the original iframe embed code.
# columns/features/lang/timeZone are display prefs for investing.com's own
# rendering -- harmless to keep even though we're parsing the HTML ourselves.
DEFAULT_PARAMS = {
    "columns": "exc_flags,exc_currency,exc_importance,exc_actual,exc_forecast,exc_previous",
    "category": "_employment,_economicActivity,_inflation,_credit,_centralBanks,_confidenceIndex,_balance,_Bonds",
    "importance": "1,2,3",  # fetch all impact levels; we filter/color-code client-side or here
    "features": "datepicker,timezone",
    "countries": "5",       # adjust/expand as needed -- see brief for the country code list
    "timeZone": "8",
    "lang": "1",
}

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

# --- Scraping logic ------------------------------------------------------

def fetch_calendar_html(cal_type: str) -> str:
    params = dict(DEFAULT_PARAMS)
    params["calType"] = cal_type  # "day" or "week"

    resp = requests.get(BASE_URL, params=params, headers=HEADERS, timeout=15)
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

            # Impact level: investing.com renders 1-3 filled "bull" icons
            # depending on importance. Count filled icons.
            impact_level = 0
            if impact_cell is not None:
                filled = impact_cell.find_all(
                    "i", {"class": lambda c: c and "GrayFullBullish" in c}
                )
                impact_level = len(filled) if filled else 0
            impact_label = IMPACT_LABELS.get(impact_level, "unknown")

            currency = ""
            if currency_cell is not None:
                span = currency_cell.find("span")
                currency = (span.get_text(strip=True) if span else
                            currency_cell.get_text(strip=True))

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
    except requests.RequestException as e:
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


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8080)
