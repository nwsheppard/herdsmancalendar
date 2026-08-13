"""
Herdsman Trading Terminal — Economic Calendar API

Scrapes the Forex Factory economic calendar (forexfactory.com/calendar) and
re-serves it as clean JSON for the ESP32 to consume.

Switched from investing.com to Forex Factory (2026-07): investing.com's
calendar carries a lot of low-value noise even after filtering (confidence
indices, minor auctions, etc.), and Forex Factory is the calendar most
traders already use day to day. Forex Factory also turned out to need less
infrastructure, not more, despite a well-known scraper for it
(github.com/fizahkhalid/forex_factory_calendar_news_scraper) using Selenium:
the calendar's event data is server-rendered in the initial HTML response
(verified directly), so the same lightweight requests+BeautifulSoup approach
already used for investing.com worked here too at the time -- no headless
Chrome/chromedriver needed in the LXC container.

That held until 2026-08: Forex Factory started sitting behind a Cloudflare
JS challenge (confirmed directly -- a plain request, even with curl_cffi's
TLS fingerprint spoofing, gets served challenges.cloudflare.com's
interstitial instead of the calendar). No amount of header/fingerprint
tuning gets past a real JS challenge, since nothing in this service
actually executes the challenge script -- fetch_calendar_html() now routes
through a self-hosted FlareSolverr instance (FLARESOLVERR_URL, required)
instead of requesting forexfactory.com directly. FlareSolverr solves the
challenge with a real headless browser and hands back the resulting HTML
over a plain HTTP API, so this service itself still doesn't bundle a
browser -- confirmed directly that the HTML it returns has the exact same
calendar__* markup as before, so parse_calendar() below needed zero
changes, only how the HTML gets fetched in the first place.

Also 2026-08: /calendar used to trigger its own FlareSolverr round trip
inline, inside whichever request happened to arrive after a short-lived
cache went stale. Reported directly, repeatedly, from the ESP32 (the
primary client): slow fetches, and outright timeouts once its own
HTTPClient budget couldn't cover FlareSolverr's worst case. Restructured so
nothing that answers a request ever talks to FlareSolverr -- a background
thread (_calendar_refresh_loop()) refreshes both ranges on its own
schedule, and /calendar always just reads whatever's currently cached, a
plain dict lookup. Any client can now poll as often as it likes without
ever paying FlareSolverr's latency itself. That schedule isn't a single
fixed interval (2026-08, refined further): CALENDAR_REFRESH_INTERVAL_LONG_S
(3h) is the steady-state cadence, switching to the much shorter
CALENDAR_REFRESH_INTERVAL_SHORT_S (5min) whenever a cached event is within
CALENDAR_EVENT_PROXIMITY_WINDOW_S of right now -- reported directly that a
flat 5-minute cadence was more load against Forex Factory than the data
(which only actually changes around events' own scheduled times)
justifies, but a long fixed interval alone would've silently broken the
ESP32's own alert_manager_tick() post-event refresh (alert_manager.cpp),
which only finds anything new if this cache happened to have refreshed
recently enough to have it. See get_cached_events()/_refresh_calendar_cache()/
_calendar_needs_short_cadence() for the mechanics, and
lifespan() for the one-time synchronous initial fetch at startup.

Also discovered at the same time (2026-08): Forex Factory geolocates an
anonymous visitor's timezone from their IP by default, not a fixed zone --
confirmed directly, this LXC's outbound IP got America/Sao_Paulo, which is
exactly what a user's "+1 hour off during EDT" report turned out to be
(Sao Paulo sits one hour ahead of New York during EDT). There's no
query-param/header override for this, only a CSRF-protected form at
/timezone -- fetch_calendar_html() now checks the fetched page's declared
timezone and, if it isn't America/New_York, drives that form once via
FlareSolverr and retries. The resulting session cookies (which pin the
timezone choice, plus Cloudflare's own cf_clearance) are persisted to
SESSION_PATH (ff_session.json, next to filters.json -- same FILTERS_DIR
volume-mount story) so this fix-and-retry round trip only happens
occasionally, not on every single fetch -- confirmed directly: a fresh
session takes one extra FlareSolverr round trip (~30s) to self-heal, a
session that already has the right cookies takes one normal round trip
(~6s), same as before this was added.

This was a full replacement, not an added option: Forex Factory has no
equivalent to investing.com's "category" concept (Employment/Inflation/
Central Banks/...), so that filter dimension is gone, not translated.
Filtering by country also became filtering by currency -- Forex Factory
organizes events by currency (9 of them: the majors plus CNY), not by
country, which is a smaller and simpler set than investing.com's 107
countries. An old filters.json from before this switch is incompatible
(numeric country codes don't map onto currency codes) and gets reset to
DEFAULT_FILTERS rather than migrated -- see load_filters().

Endpoints:
  GET  /calendar?range=day    -> today's events, filtered per filters.json
  GET  /calendar?range=week   -> this week's events (default)
  GET  /filters                -> current impact-level/currency filter selection
  POST /filters                -> update the filter selection (persisted to filters.json)
  GET  /currencies              -> all currencies Forex Factory's calendar covers, for a picker UI
  GET  /columns                 -> optional per-event fields the ESP32 can choose to display
  GET  /health                 -> simple liveness check

NOTE ON MAINTENANCE: this scrapes forexfactory.com's HTML, which can change
without notice. If events stop appearing, the first thing to check is
whether the CSS selectors below (WIDGET_TABLE_CLASS, calendar__* cell
classes, IMPACT_ICON_SUFFIX_LEVELS) still match the live page — view-source
the calendar URL and compare. Fetch failures no longer surface to clients
directly (see the background-refresh restructuring above) -- a client only
ever sees a 503 in the narrow window right after a fresh start before the
first background refresh has landed; every other failure just logs
("Background refresh of /calendar?range=... failed: ...") and keeps
serving the last good cached copy. Check this service's own logs
(`journalctl -u herdsman-calendar-api` / `docker logs`) for the real
reason, not the client response. If every refresh is failing, check
FlareSolverr itself first (is it running, is FLARESOLVERR_URL correct, can
it still solve the challenge right now -- Cloudflare's own challenge
mechanics can change too) before assuming this file's selectors are the
problem. If event times look off by a fixed offset instead
(commonly reported as "+1 hour"), that's the timezone self-heal in
fetch_calendar_html() -- check ff_session.json isn't stuck on a stale
fftimezone, or delete it to force a fresh /timezone fix on the next fetch.
"""

from fastapi import FastAPI, Query, HTTPException, Response
from fastapi.responses import JSONResponse
from pydantic import BaseModel, field_validator
from curl_cffi import requests
from bs4 import BeautifulSoup
from datetime import datetime
from pathlib import Path
from contextlib import asynccontextmanager
from urllib.parse import quote
from zoneinfo import ZoneInfo
import json
import logging
import os
import re
import threading
import time

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

    # Synchronous, not left to the background thread's first cycle: this
    # blocks startup for as long as it takes (up to ~65s x2 in the worst
    # case, both ranges needing a timezone self-heal), but that's a
    # one-time cost paid once per restart, not something every client pays
    # -- and it means /calendar has real data to serve from the moment
    # this service starts accepting requests, rather than a guaranteed
    # 503 window right after every restart. Logged but non-fatal if it
    # fails (e.g. FlareSolverr isn't reachable yet at boot) -- the
    # background loop below retries on its own regardless.
    for range in ("day", "week"):
        _refresh_calendar_cache(range)

    refresh_thread = threading.Thread(target=_calendar_refresh_loop, daemon=True, name="calendar-refresh")
    refresh_thread.start()

    yield

    _calendar_refresh_stop.set()


app = FastAPI(title="Herdsman Trading Terminal Calendar API", lifespan=lifespan)

# --- Configuration -----------------------------------------------------

BASE_URL = "https://www.forexfactory.com/calendar"
BASE_URL_ROOT = "https://www.forexfactory.com"

# The timezone the Forex Factory session is pinned to (see
# _set_forex_factory_timezone()) -- also what event day/time strings in the
# scraped HTML are expressed in, so this doubles as the zone used to parse
# them back into real timestamps (see _parse_event_datetime()).
FOREX_FACTORY_TIMEZONE_NAME = "America/New_York"
CALENDAR_TIMEZONE = ZoneInfo(FOREX_FACTORY_TIMEZONE_NAME)

# Required -- Forex Factory now sits behind a Cloudflare JS challenge
# (confirmed directly, 2026-08: a plain request, even with curl_cffi's TLS
# fingerprint spoofing, gets served challenges.cloudflare.com's interstitial
# instead of the calendar -- see fetch_calendar_html()). No compiled-in
# default, same reasoning as FILTERS_DIR/the ESP32's calendar server
# address elsewhere in this project: a FlareSolverr instance's address is
# specific to whoever's running this, not something safe to guess at.
# https://github.com/FlareSolverr/FlareSolverr -- a small self-hosted proxy
# that solves the challenge with a real headless browser and hands back the
# resulting HTML over a plain HTTP API, so this service itself still
# doesn't need to bundle a browser.
FLARESOLVERR_URL = os.environ.get("FLARESOLVERR_URL", "").rstrip("/")

# --- Filters (impact level + currencies), user-configurable via /filters ----

# Defaults to sitting next to this file (the LXC install's own behavior,
# unchanged) -- FILTERS_DIR only matters for the Docker image (see
# Docker/Dockerfile), which sets it to a dedicated directory so the whole
# directory can be volume-mounted for persistence across container
# recreations. Deliberately a directory mount, not a single file mounted
# directly onto filters.json: Docker creates a plain directory (not a
# file) at a single-file mount target that doesn't already exist as a file
# in the image, which would break FILTERS_PATH.open("w") the first time
# this container tries to save filters with nothing pre-created there yet.
FILTERS_DIR = Path(os.environ.get("FILTERS_DIR", str(Path(__file__).parent)))
FILTERS_PATH = FILTERS_DIR / "filters.json"

# Persisted Forex Factory session cookies (fftimezone, cf_clearance, etc) --
# reuses FILTERS_DIR so it survives container recreations the same way
# filters.json does, via the same volume mount. See _set_forex_factory_timezone().
SESSION_PATH = FILTERS_DIR / "ff_session.json"

# /calendar used to trigger its own FlareSolverr round trip inline, inside
# whichever request happened to arrive after a 5-minute TTL cache went
# stale (a real headless browser solving Forex Factory's Cloudflare
# challenge -- 6s with a warm session, up to 65s if the timezone self-heal
# kicks in, see fetch_calendar_html()). With the ESP32 as the primary (often
# only) client, it was consistently the one left holding that cost --
# reported directly, repeatedly, as slow fetches and outright timeouts on
# hardware with a much smaller HTTPClient timeout budget than this service
# gives itself. The underlying data only actually changes on Forex
# Factory's own schedule anyway, not on whenever a client happens to ask.
#
# Restructured so nothing that answers a request ever talks to FlareSolverr:
# a single background thread (_calendar_refresh_loop(), started from
# lifespan()) refreshes both ranges on its own clock, and /calendar always
# just reads whatever's currently cached -- a plain dict lookup, no network
# call, no wait, regardless of how slow or flaky FlareSolverr/Cloudflare are
# being at that moment. The ESP32 (or anything else) can poll as often as it
# wants now without ever paying FlareSolverr's latency itself. A refresh
# failure just leaves the previous cache entry in place untouched (see
# _refresh_calendar_cache()) -- stale-but-real beats a hard failure, and the
# next cycle tries again on its own regardless of whether anyone's asking.
#
# Two cadences, not one fixed interval: reported directly that a flat
# 5-minute cadence around the clock was more load against Forex Factory/
# FlareSolverr than the data justifies -- events only actually change
# (actual/forecast values landing, revisions) around their own scheduled
# times, not continuously. CALENDAR_REFRESH_INTERVAL_LONG_S is the
# steady-state cadence the rest of the time (still enough to catch newly
# added/removed/rescheduled events reasonably promptly); the loop switches
# to CALENDAR_REFRESH_INTERVAL_SHORT_S whenever today's cached events
# include one within CALENDAR_EVENT_PROXIMITY_WINDOW_S of right now (see
# _calendar_needs_short_cadence()). This isn't just about politeness to
# Forex Factory -- the ESP32's own alert_manager_tick() already refreshes
# 30-40s after each event's scheduled time specifically to pick up
# actual/forecast values as they land (alert_manager.cpp), and that only
# actually finds anything new if this cache has itself refreshed recently
# enough to have it -- a 3-hour blind spot around the exact moments that
# matter most would silently break that entirely.
CALENDAR_REFRESH_INTERVAL_LONG_S = 3 * 60 * 60
CALENDAR_REFRESH_INTERVAL_SHORT_S = 5 * 60
# How far before/after an event's scheduled time counts as "coming up" --
# starts the short cadence early enough to already be refreshing frequently
# going into the release, and keeps it up afterward long enough to catch a
# late-arriving actual value or a same-day revision, not just the instant
# of the release itself.
CALENDAR_EVENT_PROXIMITY_WINDOW_S = 30 * 60
_calendar_cache: dict[str, dict] = {}
# Protects concurrent dict access between the one background writer thread
# and however many request-handling threads are reading at once -- not
# guarding against concurrent *fetches* anymore, since only the background
# thread ever calls fetch_calendar_html() now.
_calendar_cache_lock = threading.Lock()
_calendar_refresh_stop = threading.Event()

DEFAULT_FILTERS = {
    "importance": [2, 3],
    "currencies": ["USD"],
    "columns": ["impact", "actual", "forecast", "previous"],
}

# Every currency Forex Factory's calendar covers -- the majors plus CNY,
# plus "All" for events that apply broadly rather than to one currency
# (e.g. OPEC meetings). Confirmed directly by scanning several months of
# the live calendar, not guessed; this is a small, stable set compared to
# investing.com's 107 countries because Forex Factory only covers
# currencies with major FX pairs, not every country's own data releases.
CURRENCY_NAMES = {
    "USD": "US Dollar",
    "EUR": "Euro",
    "GBP": "British Pound",
    "JPY": "Japanese Yen",
    "AUD": "Australian Dollar",
    "NZD": "New Zealand Dollar",
    "CAD": "Canadian Dollar",
    "CHF": "Swiss Franc",
    "CNY": "Chinese Yuan",
    "All": "All Currencies (Global Events)",
}

# Per-event data fields the ESP32 can choose to display. Unlike
# investing.com, Forex Factory has no request-level toggle for these --
# impact/actual/forecast/previous are always present in the scraped HTML
# for every event, so this is purely a display filter applied after
# parsing (see apply_column_filter()), not something that changes what's
# requested upstream.
COLUMN_NAMES = {
    "impact": "Impact",
    "actual": "Actual",
    "forecast": "Forecast",
    "previous": "Previous",
}


class FiltersUpdate(BaseModel):
    importance: list[int]
    currencies: list[str]
    columns: list[str]

    @field_validator("importance")
    @classmethod
    def validate_importance(cls, value):
        if not value or any(level not in (0, 1, 2, 3) for level in value):
            raise ValueError("importance must be a non-empty list containing only 0 (holiday), 1, 2, and/or 3")
        return value

    @field_validator("currencies")
    @classmethod
    def validate_currencies(cls, value):
        if not value or any(code not in CURRENCY_NAMES for code in value):
            raise ValueError("currencies must be a non-empty list of codes from GET /currencies")
        return value

    @field_validator("columns")
    @classmethod
    def validate_columns(cls, value):
        if not value or any(code not in COLUMN_NAMES for code in value):
            raise ValueError("columns must be a non-empty list of codes from GET /columns")
        return value


def load_filters() -> dict:
    if not FILTERS_PATH.exists():
        save_filters(DEFAULT_FILTERS)
        return dict(DEFAULT_FILTERS)
    try:
        with FILTERS_PATH.open("r", encoding="utf-8") as f:
            data = json.load(f)
        if "currencies" not in data:
            # Pre-Forex-Factory filters.json (investing.com's schema: numeric
            # "countries" instead of "currencies", plus a "categories" concept
            # Forex Factory doesn't have). There's no sane field-by-field
            # migration for this -- a country code doesn't map onto a
            # currency code -- so this resets to fresh defaults instead of
            # guessing at one.
            log.warning("%s predates the Forex Factory switch -- resetting to defaults", FILTERS_PATH)
            save_filters(DEFAULT_FILTERS)
            return dict(DEFAULT_FILTERS)
        # .get() with a default rather than data[...]: an existing
        # filters.json from before `columns` existed won't have that key yet
        # -- fall back to the default for just that, instead of treating the
        # whole file as corrupt.
        return {
            "importance": data.get("importance", DEFAULT_FILTERS["importance"]),
            "currencies": data.get("currencies", DEFAULT_FILTERS["currencies"]),
            "columns": data.get("columns", DEFAULT_FILTERS["columns"]),
        }
    except (json.JSONDecodeError, OSError) as e:
        log.warning("Failed to read %s (%s) -- falling back to defaults", FILTERS_PATH, e)
        return dict(DEFAULT_FILTERS)


def save_filters(filters: dict) -> None:
    with FILTERS_PATH.open("w", encoding="utf-8") as f:
        json.dump(filters, f)


# --- Scraping logic ------------------------------------------------------

# Selectors -- based on the calendar's known table structure. If scraping
# breaks, these are the first things to re-check against the live page.
WIDGET_TABLE_CLASS = "calendar__table"

IMPACT_LABELS = {0: "holiday", 1: "low", 2: "medium", 3: "high"}

# Maps the impact cell's icon class suffix to a level. Forex Factory doesn't
# expose a semantic title/tooltip the way investing.com's sentiment cell
# did (see git history for that scraper) -- the icon class is the only
# signal available, so it's the first thing to re-check if impact detection
# ever breaks. Confirmed directly by scanning several months of the live
# calendar, not guessed.
IMPACT_ICON_SUFFIX_LEVELS = {"gra": 0, "yel": 1, "ora": 2, "red": 3}


def _load_session_cookies() -> list:
    if not SESSION_PATH.exists():
        return []
    try:
        with SESSION_PATH.open("r", encoding="utf-8") as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        log.warning("%s is unreadable -- starting a fresh session", SESSION_PATH)
        return []


def _save_session_cookies(cookies: list) -> None:
    with SESSION_PATH.open("w", encoding="utf-8") as f:
        json.dump(cookies, f)


def _merge_cookies(base: list, new: list) -> list:
    # Last-occurrence-wins by cookie name, same de-duplication used when this
    # was worked out manually against the live site -- `new` (the response
    # from whatever request just ran) takes priority over `base` (whatever
    # was loaded/carried in), since it reflects the most current state
    # (e.g. a fresh cf_clearance replacing an expired one).
    merged = {c["name"]: c for c in base}
    merged.update({c["name"]: c for c in new})
    return list(merged.values())


def _flaresolverr_request(cmd: str, url: str, cookies: list | None = None, post_data: str | None = None) -> dict:
    payload = {"cmd": cmd, "url": url, "maxTimeout": 60000}
    if cookies:
        payload["cookies"] = cookies
    if post_data is not None:
        payload["postData"] = post_data

    resp = requests.post(
        f"{FLARESOLVERR_URL}/v1",
        json=payload,
        # FlareSolverr's own budget is maxTimeout above (60s) -- this needs
        # to be a bit longer than that, not equal to it, so a solve that
        # takes the full internal budget doesn't also get cut off by this
        # library's own timeout right as FlareSolverr was about to respond.
        timeout=65,
    )
    resp.raise_for_status()
    data = resp.json()

    if data.get("status") != "ok":
        raise RuntimeError(f"FlareSolverr couldn't fetch {url}: {data.get('message')}")

    solution = data.get("solution", {})
    upstream_status = solution.get("status")
    if upstream_status and upstream_status != 200:
        raise RuntimeError(f"FlareSolverr reached {url}, but it returned HTTP {upstream_status}")

    return solution


def _set_forex_factory_timezone(cookies: list) -> list:
    # Forex Factory defaults anonymous visitors to an IP-geolocated
    # timezone (confirmed directly -- this LXC's outbound IP got
    # America/Sao_Paulo, not America/New_York, exactly explaining a
    # reported "+1 hour" offset during EDT). There's no query-param or
    # header override -- the only way to change it is this CSRF-protected
    # form, confirmed directly by working through the live flow by hand:
    # GET /timezone for a fresh CSRF token, then POST it back with the
    # desired zone.
    tz_page = _flaresolverr_request("request.get", f"{BASE_URL_ROOT}/timezone", cookies=cookies)
    csrf_match = re.search(r'name="_csrf" value="([a-f0-9]+)"', tz_page.get("response", ""))
    if not csrf_match:
        raise RuntimeError("Couldn't find Forex Factory's _csrf token on /timezone -- page layout may have changed")

    cookies_after_get = _merge_cookies(cookies, tz_page.get("cookies", []))
    post_solution = _flaresolverr_request(
        "request.post",
        f"{BASE_URL_ROOT}/timezone",
        cookies=cookies_after_get,
        post_data=f"_csrf={csrf_match.group(1)}&timezone={quote(FOREX_FACTORY_TIMEZONE_NAME, safe='')}",
    )
    return _merge_cookies(cookies_after_get, post_solution.get("cookies", []))


def fetch_calendar_html(cal_type: str) -> str:
    # Forex Factory accepts literal "today"/"this" aliases directly (verified
    # directly) -- no need to compute or pass an actual date string.
    param = "day=today" if cal_type == "day" else "week=this"
    url = f"{BASE_URL}?{param}"

    if not FLARESOLVERR_URL:
        raise RuntimeError(
            "FLARESOLVERR_URL is not configured -- Forex Factory now requires "
            "a Cloudflare JS challenge to be solved before it'll serve the "
            "calendar (confirmed directly, 2026-08), so this service can't "
            "fetch anything without a FlareSolverr instance to route through. "
            "See README's Maintenance notes."
        )

    # Previously a direct `requests.get(url, headers=HEADERS, impersonate="chrome")`
    # -- Forex Factory didn't sit behind Cloudflare bot management when that
    # was written, confirmed directly at the time. It does now (also
    # confirmed directly: a plain request gets served
    # challenges.cloudflare.com's interstitial instead of the calendar, even
    # with curl_cffi's TLS fingerprint spoofing) -- curl_cffi can't get past
    # a real JS challenge no matter what it impersonates, since nothing
    # actually executes the challenge script. FlareSolverr does that part
    # with a real headless browser and hands back the resulting HTML, so
    # parse_calendar() below needs no changes at all -- confirmed directly,
    # the returned HTML contains the exact same calendar__* markup as
    # before, this only changes how it's fetched.
    cookies = _load_session_cookies()
    solution = _flaresolverr_request("request.get", url, cookies=cookies)
    html = solution.get("response", "")
    cookies = _merge_cookies(cookies, solution.get("cookies", []))

    # Anonymous visitors get an IP-geolocated timezone by default (see
    # _set_forex_factory_timezone()) -- self-heal it once here rather than
    # requiring a one-off manual fix, and persist the resulting cookies so
    # this only runs again if the session's timezone ever reverts (a fresh
    # cf_clearance without fftimezone, an expired session, etc), not on
    # every single fetch.
    tz_match = re.search(r"timezone_name[\"']?\s*[:=]\s*[\"']([^\"']+)", html)
    if not tz_match or tz_match.group(1) != FOREX_FACTORY_TIMEZONE_NAME:
        log.info("Forex Factory session timezone is %s, not %s -- fixing it",
                  tz_match.group(1) if tz_match else "unknown", FOREX_FACTORY_TIMEZONE_NAME)
        cookies = _set_forex_factory_timezone(cookies)
        solution = _flaresolverr_request("request.get", url, cookies=cookies)
        html = solution.get("response", "")
        cookies = _merge_cookies(cookies, solution.get("cookies", []))

        tz_match = re.search(r"timezone_name[\"']?\s*[:=]\s*[\"']([^\"']+)", html)
        if not tz_match or tz_match.group(1) != FOREX_FACTORY_TIMEZONE_NAME:
            raise RuntimeError(
                f"Set Forex Factory's timezone to {FOREX_FACTORY_TIMEZONE_NAME} but the calendar still shows "
                f"{tz_match.group(1) if tz_match else 'no timezone at all'} -- not retrying again to avoid looping forever"
            )

    _save_session_cookies(cookies)
    return html


def parse_value_state(cell) -> str:
    """
    "better"/"worse"/"neutral" from a value cell's wrapper <span> class --
    Forex Factory pre-computes this itself (accounting for which direction
    is actually "good" for a given indicator, e.g. higher unemployment is
    bad but higher GDP is good), so this reads their judgment rather than
    guessing one from the raw numbers. Confirmed directly against
    forexfactory.com's own stylesheet: `.better{color:#090}`,
    `.worse{color:#c00}`. Used for both `actual` (vs forecast) and
    `previous` (vs its originally-reported value, when revised) -- the
    wrapper span's class shape is identical for both.
    """
    if cell is None:
        return "neutral"
    span = cell.find("span")
    if span is None:
        return "neutral"
    classes = span.get("class") or []
    if "worse" in classes:
        return "worse"
    if "better" in classes:
        return "better"
    return "neutral"


def parse_previous_revised(previous_cell) -> bool:
    """Whether this period's "previous" value was revised from what was originally reported last time."""
    if previous_cell is None:
        return False
    span = previous_cell.find("span")
    return span is not None and "revised" in (span.get("class") or [])


def parse_impact_level(impact_cell) -> int:
    """Impact level from the impact cell's icon class suffix -- see IMPACT_ICON_SUFFIX_LEVELS."""
    if impact_cell is None:
        return 0

    # BeautifulSoup's class-filter callback is invoked once per individual
    # class token AND once with the full space-joined string -- a lambda
    # expecting a list here (e.g. `any(cls.startswith(...) for cls in c)`)
    # silently iterates the wrong thing (characters of a string) on every
    # call and never matches. A plain substring check on the (sometimes
    # single-token, sometimes joined) string handles all of bs4's calls
    # correctly -- verified directly, this exact gotcha broke the first
    # version of this function.
    icon = impact_cell.find("span", {"class": lambda c: c and "icon--ff-impact-" in c})
    if icon is None:
        return 0

    for cls in icon.get("class", []):
        if cls.startswith("icon--ff-impact-"):
            suffix = cls[len("icon--ff-impact-"):]
            return IMPACT_ICON_SUFFIX_LEVELS.get(suffix, 0)
    return 0


def parse_calendar(html: str) -> list[dict]:
    soup = BeautifulSoup(html, "html.parser")
    table = soup.find("table", {"class": WIDGET_TABLE_CLASS})
    if table is None:
        # Selector likely stale -- surface a clear error rather than
        # silently returning nothing.
        raise RuntimeError(
            f"Could not find table.{WIDGET_TABLE_CLASS} in response -- "
            "Forex Factory's markup may have changed. Check selectors."
        )

    rows = table.find_all("tr")
    events = []

    current_day = None
    current_time = ""

    for row in rows:
        classes = row.get("class") or []
        if "calendar__row" not in classes:
            # Header/subhead/borderfix rows -- not an event or day divider.
            continue
        if "calendar__row--day-breaker" in classes:
            # Empty visual divider between days, no data of its own.
            continue

        try:
            # The date cell is only populated on the first row of a new day
            # (a "calendar__row--new-day" row) -- blank on every row after
            # that for the same day, so the last non-blank value carries
            # forward. Same story for the time cell within a day: blank on
            # every row after the first at a given time (grouped same-time
            # events aren't repeated).
            date_cell = row.find("td", {"class": "calendar__date"})
            if date_cell is not None:
                # The weekday abbreviation and "Mon DD" live in separate
                # sibling elements with no whitespace text node between
                # them in Forex Factory's markup -- get_text(strip=True)
                # alone concatenates them with nothing in between
                # ("FriJul 31", confirmed directly against the live site).
                # separator=" " fixes that but can double up wherever a
                # real space already existed between fragments, so collapse
                # runs of whitespace down to one afterward rather than
                # assume the exact tag structure.
                day_text = re.sub(r"\s+", " ", date_cell.get_text(separator=" ", strip=True)).strip()
                if day_text:
                    current_day = day_text

            time_cell = row.find("td", {"class": "calendar__time"})
            if time_cell is not None:
                time_text = time_cell.get_text(strip=True)
                if time_text:
                    current_time = time_text

            currency_cell = row.find("td", {"class": "calendar__currency"})
            impact_cell = row.find("td", {"class": "calendar__impact"})
            event_cell = row.find("td", {"class": "calendar__event"})
            actual_cell = row.find("td", {"class": "calendar__actual"})
            forecast_cell = row.find("td", {"class": "calendar__forecast"})
            previous_cell = row.find("td", {"class": "calendar__previous"})

            if event_cell is None:
                continue

            event_title = event_cell.find("span", {"class": "calendar__event-title"})
            name = event_title.get_text(strip=True) if event_title else event_cell.get_text(strip=True)
            if not name:
                continue

            impact_level = parse_impact_level(impact_cell)

            # Forex Factory's own stable per-event ID (data-event-id on the
            # row) -- lets the ESP32 track "have I already alerted/refreshed
            # for this specific event" across repeated fetches, instead of
            # matching on name+time (fragile: not unique, and both are
            # display strings that could shift between requests).
            event_id = int(row.get("data-event-id", 0) or 0)

            events.append({
                "id": event_id,
                "day": current_day,
                "time": current_time,
                "currency": currency_cell.get_text(strip=True) if currency_cell else "",
                "impact": IMPACT_LABELS.get(impact_level, "unknown"),
                "impact_level": impact_level,
                "name": name,
                "actual": actual_cell.get_text(strip=True) if actual_cell else "",
                "actual_state": parse_value_state(actual_cell),
                "forecast": forecast_cell.get_text(strip=True) if forecast_cell else "",
                "previous": previous_cell.get_text(strip=True) if previous_cell else "",
                "previous_state": parse_value_state(previous_cell),
                "previous_revised": parse_previous_revised(previous_cell),
            })
        except Exception as e:
            # Don't let one malformed row kill the whole response.
            log.warning("Skipping malformed row: %s", e)
            continue

    return events


def apply_column_filter(events: list[dict], columns: list[str]) -> list[dict]:
    """
    Blank out fields the user's `columns` selection excludes. Applied after
    parsing/importance-and-currency filtering, since impact_level is needed
    for importance filtering regardless of whether "impact" itself ends up
    excluded from the response.
    """
    for event in events:
        if "impact" not in columns:
            event["impact"] = "unknown"
            event["impact_level"] = 0
        if "actual" not in columns:
            event["actual"] = ""
            event["actual_state"] = "neutral"
        if "forecast" not in columns:
            event["forecast"] = ""
        if "previous" not in columns:
            event["previous"] = ""
            event["previous_state"] = "neutral"
            event["previous_revised"] = False
    return events


_MONTH_ABBREVS = {
    "jan": 1, "feb": 2, "mar": 3, "apr": 4, "may": 5, "jun": 6,
    "jul": 7, "aug": 8, "sep": 9, "oct": 10, "nov": 11, "dec": 12,
}
_EVENT_TIME_RE = re.compile(r"^(\d{1,2}):(\d{2})(am|pm)$")


def _parse_event_datetime(day: str, time_str: str, reference: datetime) -> datetime | None:
    """
    Parses an event's "day" ("Fri Jul 27") + "time" ("8:30am") fields into
    an aware datetime in CALENDAR_TIMEZONE -- mirrors the ESP32's own
    alert_manager.cpp parse_event_timestamp() field-by-field, so "does this
    event count as happening soon" means the same thing on both sides.
    Returns None for events with no fixed time ("All Day"/"Tentative"/"")
    or malformed day text, same as that function.
    """
    if not day or not time_str:
        return None

    # Skip the weekday abbreviation ("Fri Jul 27" -> "Jul 27").
    day_parts = day.split(None, 1)
    if len(day_parts) != 2:
        return None
    month_and_day = day_parts[1].split()
    if len(month_and_day) != 2:
        return None
    month = _MONTH_ABBREVS.get(month_and_day[0].lower())
    if month is None or not month_and_day[1].isdigit():
        return None
    day_num = int(month_and_day[1])

    time_match = _EVENT_TIME_RE.match(time_str.strip().lower())
    if not time_match:
        return None
    hour, minute, meridiem = int(time_match[1]), int(time_match[2]), time_match[3]
    if not (1 <= hour <= 12 and 0 <= minute <= 59):
        return None
    if meridiem == "pm" and hour != 12:
        hour += 12
    if meridiem == "am" and hour == 12:
        hour = 0

    year = reference.year
    # The only case a day/week-range fetch can straddle a year boundary:
    # today is December and the event's month is January, so it must be
    # next year, not this one -- same reasoning as the ESP32's own
    # parse_event_timestamp().
    if reference.month == 12 and month == 1:
        year += 1

    try:
        return datetime(year, month, day_num, hour, minute, tzinfo=CALENDAR_TIMEZONE)
    except ValueError:
        return None


def _calendar_needs_short_cadence(now: datetime) -> bool:
    """
    True if any currently-cached "day" event falls within
    CALENDAR_EVENT_PROXIMITY_WINDOW_S of `now` -- see
    CALENDAR_REFRESH_INTERVAL_SHORT_S's own comment for why. Only the "day"
    range is checked: `now` is always within today, so any event close
    enough to matter is necessarily in that range regardless of what
    "week" also contains.
    """
    with _calendar_cache_lock:
        cached = _calendar_cache.get("day")
    if cached is None:
        return False

    for event in cached["events"]:
        event_dt = _parse_event_datetime(event.get("day", ""), event.get("time", ""), now)
        if event_dt is None:
            continue
        if abs((event_dt - now).total_seconds()) <= CALENDAR_EVENT_PROXIMITY_WINDOW_S:
            return True
    return False


def _refresh_calendar_cache(range: str) -> None:
    """
    Fetches + parses `range` and updates the cache -- called by
    _calendar_refresh_loop() on its own schedule, and once more directly
    from lifespan() at startup so the service has real data before it
    accepts its first request. Never called from inside a request handler.

    On failure, logs and returns without touching the cache -- whatever was
    there before (possibly nothing, right after a fresh start) is left
    exactly as it was, so get_cached_events() keeps serving the last good
    copy rather than the request path ever seeing the failure directly.
    """
    try:
        html = fetch_calendar_html(range)
        events = parse_calendar(html)
    except requests.exceptions.RequestException as e:
        log.error("Background refresh of /calendar?range=%s failed: %s", range, e)
        return
    except RuntimeError as e:
        log.error("Background refresh of /calendar?range=%s failed to parse: %s", range, e)
        return

    with _calendar_cache_lock:
        _calendar_cache[range] = {"events": events, "fetched_at": time.monotonic()}
    log.info("Refreshed /calendar?range=%s: %d events", range, len(events))


def _calendar_refresh_loop() -> None:
    """
    Runs for the life of the process (started as a daemon thread from
    lifespan(), after lifespan()'s own initial synchronous refresh).

    Waits before each refresh, not after -- lifespan() already did the
    first fetch for both ranges before this thread was even started;
    refreshing again immediately here would just repeat that same round
    trip a second time for no reason. Event.wait() as the loop condition
    (rather than wait() as a plain statement inside the loop) is what makes
    "wait first" and "stop promptly on shutdown" both fall out naturally:
    it returns True the moment the stop Event is set, so a shutdown
    mid-wait exits the loop immediately instead of sleeping out the full
    interval first.

    The wait itself is CALENDAR_REFRESH_INTERVAL_SHORT_S or _LONG_S,
    decided fresh each time from whatever's currently cached (i.e. as of
    the *previous* refresh, not this upcoming one -- see
    _calendar_needs_short_cadence()'s own comment for why that's fine: an
    event just outside the window on this check will be well inside it by
    the next one either way, since the short cadence is much shorter than
    the proximity window itself).
    """
    while True:
        now = datetime.now(CALENDAR_TIMEZONE)
        interval = (
            CALENDAR_REFRESH_INTERVAL_SHORT_S
            if _calendar_needs_short_cadence(now)
            else CALENDAR_REFRESH_INTERVAL_LONG_S
        )
        if _calendar_refresh_stop.wait(interval):
            return
        for range in ("day", "week"):
            _refresh_calendar_cache(range)


def get_cached_events(range: str) -> list[dict]:
    """
    Cached, unfiltered events for `range` -- a plain dict read, never a
    network call (see _calendar_refresh_loop()). Filters are intentionally
    not baked into the cache (see get_calendar()) -- they're cheap to apply
    per-request, and caching post-filter would mean a filter change doesn't
    take effect until the next background refresh.
    """
    with _calendar_cache_lock:
        cached = _calendar_cache.get(range)

    if cached is None:
        # Only possible in the brief window right after a fresh start,
        # before lifespan()'s own initial synchronous refresh has landed
        # (or if that refresh itself failed -- e.g. FlareSolverr wasn't
        # reachable yet at boot). The background loop keeps retrying
        # regardless; check this service's own logs for the actual reason
        # if this persists past a normal startup.
        raise HTTPException(status_code=503, detail="Calendar data isn't loaded yet -- try again shortly")

    return cached["events"]


# --- API endpoints ---------------------------------------------------------

@app.get("/health")
def health():
    return {"status": "ok", "time": datetime.utcnow().isoformat()}


@app.get("/favicon.ico", include_in_schema=False)
def favicon():
    # This is a JSON API with no static assets of its own -- nothing to
    # actually serve here. Without this route, a browser hitting any
    # endpoint directly (testing /calendar in a tab, etc.) still fires its
    # own automatic /favicon.ico request, which fell through to FastAPI's
    # default 404 handler. Explicit 204 instead: same "nothing here" result,
    # but skips routing through the 404 handler and settles the browser's
    # request immediately rather than leaving it to time out/error in devtools.
    return Response(status_code=204)


@app.get("/calendar")
def get_calendar(range: str = Query("week", pattern="^(day|week)$")):
    """
    range=day  -> today's events only
    range=week -> this week's events (default)
    """
    filters = load_filters()
    events = get_cached_events(range)

    # Forex Factory has no server-side filtering by impact/currency the way
    # investing.com's importance=/countries= params did -- day/week is the
    # only thing its own URL controls. Importance/currency selection is
    # applied here instead, after scraping the full unfiltered response.
    #
    # dict(e), not e itself: events now comes from get_cached_events()'s
    # shared cache, not a fresh parse per request -- apply_column_filter()
    # below mutates each event dict in place (blanking excluded columns),
    # which used to be harmless when every request got its own freshly
    # parsed list. Against the cache, that mutation would corrupt it for
    # every other request sharing that entry (e.g. one client excluding
    # "actual" permanently blanking it for everyone else until the next
    # real fetch) -- copying here keeps the cached originals untouched.
    events = [
        dict(e) for e in events
        if e["impact_level"] in filters["importance"] and e["currency"] in filters["currencies"]
    ]
    events = apply_column_filter(events, filters["columns"])

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
    data = {
        "importance": filters.importance,
        "currencies": filters.currencies,
        "columns": filters.columns,
    }
    save_filters(data)
    log.info("Filters updated: %s", data)
    return data


@app.get("/currencies")
def get_currencies():
    return [
        {"code": code, "name": name}
        for code, name in sorted(CURRENCY_NAMES.items(), key=lambda item: item[1])
    ]


@app.get("/columns")
def get_columns():
    return [{"code": code, "name": name} for code, name in COLUMN_NAMES.items()]


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8080)
