# Herdsman Calendar API

A small FastAPI service that scrapes the Forex Factory economic calendar
and serves clean JSON for an ESP32-based terminal client.

This folder follows the same two-phase pattern used by
[Proxmox VE Helper-Scripts](https://github.com/community-scripts/ProxmoxVE)
(e.g. [`ct/bazarr.sh`](https://github.com/community-scripts/ProxmoxVE/blob/main/ct/bazarr.sh) /
[`install/bazarr-install.sh`](https://github.com/community-scripts/ProxmoxVE/blob/main/install/bazarr-install.sh)):
a host-side script creates the LXC, and a container-side script provisions it.

`install.sh` sources community-scripts' real `misc/build.func` directly, so
running it gives you their actual interactive experience: the Default
Install / Advanced Install / Settings whiptail menu, storage and resource
prompts, template selection, all of it. The only thing rewritten in flight
is the single line inside `build.func` that fetches the per-app install
script -- that line is hardcoded to community-scripts' own repo, so it's
redirected (via a `sed` in the `source <(curl ... | sed ...)` line) to this
repo's `herdsman-calendar-install.sh` instead. Everything else (`core.func`,
`error_handler.func`, `tools.func`) loads unmodified straight from their
repo on every run, so there's nothing vendored here to fall out of date.

## What this repository contains

- `calendar_api.py` — the scraper and API service
- `requirements.txt` — Python dependencies
- `install.sh` — run on the Proxmox VE host; sources community-scripts'
  `build.func` and drives the interactive container creation
- `herdsman-calendar-install.sh` — run inside the LXC by `build.func`;
  installs Python, fetches the app, creates the systemd service, and starts it

## Before you start: FlareSolverr is required

Forex Factory sits behind a Cloudflare JS challenge as of 2026-08 (see
`calendar_api.py`'s module docstring) -- the API can't fetch the calendar
at all without a [FlareSolverr](https://github.com/FlareSolverr/FlareSolverr)
instance to route the request through. It isn't installed by this script
(it needs a real headless browser, much heavier than this LXC's own 512MB
default) -- run it separately (its own LXC/container/VM, wherever's
convenient, as long as this container can reach it), then either:

- Fresh install: `export FLARESOLVERR_URL=http://<host>:8191` before
  running the install command below -- picked up automatically *if*
  community-scripts' `build.func` happens to forward it through to the
  container-side script (not guaranteed either way; the install finishes
  successfully regardless, and prints a clear reminder at the end if it
  didn't take).
- Already installed: `pct enter <ctid>`, then
  `FLARESOLVERR_URL=http://<host>:8191 update` -- see "Updating" below.
  Same result as a manual `systemctl edit`, no separate step needed.
- Or set it directly, any time (fresh install or already installed):
  ```bash
  pct exec <ctid> -- systemctl edit herdsman-calendar-api
  # add under [Service]:
  #   Environment=FLARESOLVERR_URL=http://<your-flaresolverr-host>:8191
  pct exec <ctid> -- systemctl restart herdsman-calendar-api
  ```

Without this, the service still starts, but its background calendar
refresh (see "How /calendar stays fast" below) can never actually
succeed -- `journalctl -u herdsman-calendar-api -f` shows a clear
`FLARESOLVERR_URL is not configured...` error on every refresh attempt.
`/calendar` itself won't show that error directly to a client; it'll
either serve nothing yet (a 503 right after a fresh start) or, if it had
previously fetched real data before `FLARESOLVERR_URL` went missing,
keep serving that increasingly stale copy indefinitely. Check the logs,
not the client response, if you're not sure whether it's set.

## Quick install

Run directly on a Proxmox VE host:

```bash
bash -c "$(curl -fsSL https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC/install.sh)"
```

You'll get the same "Default Install / Advanced Install / Settings" menu
Proxmox Helper Scripts normally show. Default Install uses the resource
defaults declared in `install.sh` (1 core, 512 MB RAM, 4 GB disk, Debian 12,
unprivileged); Advanced Install lets you override CPU/RAM/disk/storage/
network/CTID interactively.

## Updating

Proxmox Helper Scripts update in-container, not by re-running the host
script: `pct enter <ctid>`, then run `update`. That re-invokes `install.sh`'s
`update_script()` function, which pulls the latest `calendar_api.py` /
`requirements.txt`, reinstalls dependencies, and restarts the service.

`update` also doubles as a way to set (or change) `FLARESOLVERR_URL`
without a separate `systemctl edit` step -- export it first:

```bash
FLARESOLVERR_URL=http://<your-flaresolverr-host>:8191 update
```

Safe to run this way every time regardless of whether it's already set --
it writes to the same drop-in file `systemctl edit` itself would use
(`override.conf`), so there's never two separate overrides fighting over
the same setting, and re-writing the same value is a no-op. Leaving
`FLARESOLVERR_URL` unset and just running plain `update` leaves whatever's
already configured untouched (doesn't reset it to empty) -- `update`
prints a clear reminder at the end only if it finds the service's
*effective* environment still has nothing configured, checked after the
restart, not just whether this particular run happened to set it.

## Testing the API

```bash
curl http://<container-ip>:8080/health
curl http://<container-ip>:8080/calendar?range=day
curl http://<container-ip>:8080/calendar?range=week
curl "http://<container-ip>:8080/calendar/wait?range=day&since=-1"   # instant -- since=-1 always "changed"
curl "http://<container-ip>:8080/calendar/wait?range=day&since=1"    # blocks up to CALENDAR_LONG_POLL_TIMEOUT_S
```

## How /calendar stays fast

`/calendar` never talks to FlareSolverr itself -- a background thread
refreshes both `day` and `week` caches on its own schedule
(`calendar_api.py`), and the endpoint just reads whatever's currently
cached, a plain dict lookup. Poll it as often as you like (the ESP32
defaults to every 10 minutes); nothing a client does ever triggers a
FlareSolverr round trip or waits on one.

That schedule isn't a single fixed interval: `CALENDAR_REFRESH_INTERVAL_LONG_S`
(3 hours) is the steady-state cadence, switching to the much shorter
`CALENDAR_REFRESH_INTERVAL_SHORT_S` (5 minutes) whenever a cached event's
scheduled time is within `CALENDAR_EVENT_PROXIMITY_WINDOW_S` (30 minutes)
of right now. A flat 5-minute cadence around the clock turned out to be
more load against Forex Factory than the data justifies -- events only
actually change (actual/forecast values landing, revisions) around their
own scheduled times, not continuously -- but a long fixed interval alone
would've silently broken the ESP32's own `alert_manager_tick()`
post-event refresh (`alert_manager.cpp`), which only finds anything new
if this cache happened to have refreshed recently enough to have it.

This wasn't always true, and mattered a lot in practice: it used to fetch
inline, on whichever request happened to arrive after a short-lived cache
went stale -- fine when the calendar loaded quickly, but once Forex
Factory's Cloudflare challenge (below) made every fetch take several
seconds, up to 65s in the worst case, the ESP32 (as the primary, often
only, client) was the one left holding that cost on every trigger,
repeatedly reported as slow loads and outright timeouts on hardware with
a much smaller HTTPClient budget than this service gives itself.

One consequence: this service's own startup now blocks for as long as
the first fetch takes (up to ~65s x2 in the worst case, both ranges
needing a timezone self-heal) -- `systemctl start`/`docker compose up`
won't report ready until that's done. That's a one-time cost per restart,
not something every client pays, and it means `/calendar` has real data
to serve from the moment the service starts accepting requests rather
than a guaranteed empty window right after every restart.

A background refresh that fails (FlareSolverr down, Forex Factory
serving something unexpected, etc.) just logs and leaves the previous
cache entry untouched -- stale-but-real beats a hard failure, and the
next cycle tries again on its own regardless of whether anyone's asking.
The only time a client sees an actual error is a 503 in the narrow window
right after a fresh start, before the first background refresh has
landed at all.

## Push updates: /calendar/wait

`/calendar` always answers immediately from the cache -- fine for a quick
check, but a client that wants updates *as they happen*, without deciding
on its own polling schedule, should use `/calendar/wait?range=day&since=<version>`
instead. It blocks server-side for up to `CALENDAR_LONG_POLL_TIMEOUT_S`
(25s) until `range`'s cache version actually differs from `since`, then
returns the same shape `/calendar` does, plus a `version` field. Pass
`since=-1` for a first call (or any time you want the current data
immediately, no waiting) -- calendar_api.py treats that as a version that
can never match, so it always returns right away. After that, pass back
whatever `version` you last received to keep waiting for the next real
change.

This is what the ESP32 firmware actually uses now (see
`calendar_view.cpp`'s `refresh_task()`/`apply_ready_refresh_result()`) --
no WebSockets, no persistent connection to manage, just the same plain
HTTP GET repeated in a loop, one request immediately following the last.
A change on the backend (a new event, an actual/forecast value landing)
reaches the screen within moments of the background thread picking it up,
not up to a full poll interval later.

A version only bumps when the underlying events actually change -- a
background refresh that finds nothing new (the common case) doesn't wake
any waiters. Changing filters via Settings doesn't bump it either (the
same underlying events, just filtered differently), which is why the
ESP32 always issues an immediate `since=-1` request right after a
Settings change rather than waiting for its current long-poll to resolve.

## Data source: Forex Factory

Switched from investing.com to Forex Factory (2026-07) -- investing.com's
calendar carries a lot of low-value noise even after filtering (confidence
indices, minor auctions, etc.), and Forex Factory is the calendar most
traders already use. This was a full replacement, not an added option:
investing.com's scraper is gone, not kept as a fallback.

A well-known scraper for Forex Factory
([fizahkhalid/forex_factory_calendar_news_scraper](https://github.com/fizahkhalid/forex_factory_calendar_news_scraper))
uses Selenium, which would have meant a much heavier LXC container (a real
or headless Chrome binary + chromedriver, hundreds of MB, much slower per
request). That turned out to be unnecessary at the time: Forex Factory's
calendar event data is server-rendered in the initial HTML response, and
back in 2026-07 the site didn't sit behind Cloudflare bot management the
way investing.com did -- a plain `requests` call got a normal 200
(verified directly at the time). **That held only until 2026-08** --
Forex Factory added a Cloudflare JS challenge, and this service now
depends on a separately-run FlareSolverr instance to get past it (see
"Before you start" above) -- the exact heavier-infrastructure outcome this
section originally said turned out to be unnecessary. `curl_cffi` stays in
`requirements.txt` for the request *to FlareSolverr itself*, not because
its TLS fingerprint spoofing still does anything against Forex
Factory directly -- it doesn't, a real JS challenge doesn't care what a
request's TLS handshake looks like.

Forex Factory organizes events by **currency** (9 of them: the majors plus
CNY, plus "All" for events that apply broadly rather than to one currency,
e.g. OPEC meetings) rather than by country, and has **no equivalent to
investing.com's "category" concept** (Employment/Inflation/Central Banks/
...) -- events are just currency + impact + name. Both of those are
reflected in the filter schema below: countries became a much smaller
currencies list, and categories are gone entirely rather than translated
into something Forex Factory doesn't actually support.

Forex Factory's impact levels also have a 4th tier investing.com's didn't:
grey ("holiday", non-economic notices like bank holidays), on top of the
usual yellow/orange/red (low/medium/high). This maps directly onto the kind
of low-value clutter the switch was meant to get away from -- it's a real,
selectable `importance` level (0), just excluded from `DEFAULT_FILTERS` by
default.

## Filters: impact level, currency, and columns

`importance` (which impact levels to show, including holiday/0) and
`currencies` (which currencies) aren't fixed in code — they live in
`filters.json`, created next to `calendar_api.py` with defaults of
`{"importance": [2, 3], "currencies": ["USD"], "columns": ["impact",
"actual", "forecast", "previous"]}` the moment the service starts (a
FastAPI `lifespan` hook calls `load_filters()` on startup) — not lazily on
the first `/calendar` or `/filters` request, which is surprising to find
missing if you go looking for it right after `systemctl start`/`update`.
This file survives `update` redeploys, since those only overwrite
`calendar_api.py`/`requirements.txt`.

A second file, `ff_session.json`, lives next to it for the same reason --
Forex Factory geolocates an anonymous visitor's timezone from their IP by
default (confirmed directly, 2026-08: this project's own outbound IP got
America/Sao_Paulo, not America/New_York -- exactly a "+1 hour off" during
EDT), so `fetch_calendar_html()` self-heals it by driving Forex Factory's
`/timezone` form once and persisting the resulting session cookies here.
Nothing to configure -- it's created automatically on the first `/calendar`
fetch and reused after that, so only the first fetch after a fresh install
(or after deleting this file) takes noticeably longer (~30s vs ~6s) while
it fixes the timezone once.

`columns` (which optional per-event fields -- Impact/Actual/Forecast/
Previous -- to display) works differently here than it did against
investing.com: Forex Factory has no request-level toggle for these fields,
they're always present in the scraped HTML for every event, so `columns`
is applied as a post-scrape display filter (`apply_column_filter()`) rather
than changing what's requested upstream. currency/time/name are always
present and not part of this list.

An old, pre-switch `filters.json` (investing.com's schema: numeric
`countries` instead of `currencies`, plus a `categories` key Forex Factory
has no equivalent for) is detected and reset to `DEFAULT_FILTERS` rather
than migrated field-by-field -- a country code doesn't map onto a currency
code, so there's no sane way to carry old values forward automatically.

This makes each deployment independently configurable — useful since
different customers of the same product want different things (some want
low-impact events too, some want EUR/JPY alongside USD, some don't care
about Forecast/Previous values, etc.) without needing a per-customer server
config or code change. The ESP32's Settings screen is the intended way to
change this day-to-day; the endpoints below are what it calls.

```bash
# Current selection
curl http://<container-ip>:8080/filters

# All currencies Forex Factory's calendar covers, for building a picker UI
curl http://<container-ip>:8080/currencies

# All optional per-event columns the ESP32 can choose to show
curl http://<container-ip>:8080/columns

# Update the selection (validated: importance must be 0/1/2/3, currencies
# must be codes from GET /currencies, columns from GET /columns -- an
# invalid request leaves the previously saved filters untouched rather than
# partially applying)
curl -X POST http://<container-ip>:8080/filters \
  -H "Content-Type: application/json" \
  -d '{"importance": [1, 2, 3], "currencies": ["USD", "EUR", "JPY"],
       "columns": ["impact", "actual"]}'
```

Currency codes (`GET /currencies`) were confirmed directly by scanning
several months of the live calendar, not guessed or pulled from a
third-party list -- Forex Factory doesn't publish this set anywhere else,
so re-scan the live site if a new currency is ever suspected.

## `/calendar` event shape: stable IDs and actual/previous value coloring

Beyond the fields already covered above (`day`/`time`/`currency`/`impact`/
`impact_level`/`name`/`actual`/`forecast`/`previous`), each event also
carries:

- `id` -- Forex Factory's own stable per-event ID (the `data-event-id`
  attribute already present on every event's `<tr>`). Lets a client track
  "have I already alerted/refreshed for this specific event" across
  repeated fetches without matching on name+time (fragile: neither is
  guaranteed unique, and both can shift between requests).
- `actual_state` / `previous_state` -- `"better"`, `"worse"`, or
  `"neutral"`. Forex Factory pre-computes this itself (accounting for
  which direction is actually good for a given indicator -- higher
  unemployment is bad, higher GDP is good -- something this scraper has no
  way to know per-indicator on its own) and marks it via a wrapper `<span>`
  class on the `actual`/`previous` cells; this just reads that judgment
  rather than deriving one from the raw numbers. Colors match
  forexfactory.com's own stylesheet exactly, confirmed directly:
  `.better{color:#090}`, `.worse{color:#c00}`.
- `previous_revised` -- whether this period's `previous` value was revised
  from what was originally reported last time (a separate `<span>` class,
  independent of worse/better -- a revision can be neutral, better, or
  worse).

All three reset to their neutral defaults when `columns` excludes `actual`/
`previous` (`apply_column_filter()`), consistent with those fields
themselves being blanked.

## Maintenance notes

- If `/calendar` is serving stale or empty data (see "How /calendar stays
  fast" above -- a background refresh failure never surfaces as a client
  error, only stale data or, right after a fresh start, a 503), check
  `journalctl -u herdsman-calendar-api -f` for
  `Background refresh of /calendar?range=... failed: ...` lines, then
  check FlareSolverr before assuming `calendar_api.py` broke: is it
  running, is `FLARESOLVERR_URL` set correctly on this service
  (`systemctl cat herdsman-calendar-api` shows the effective config,
  including any `systemctl edit` drop-in), and can it still solve Forex
  Factory's challenge right now -- Cloudflare's own challenge mechanics
  change too, independent of anything in this repo.
- If event times look off by a fixed offset (commonly "+1 hour" during
  EDT), that's Forex Factory's IP-geolocated timezone default, not a bug in
  this scraper -- `fetch_calendar_html()` should self-heal it automatically
  via `/opt/herdsman-calendar/ff_session.json`. If it's stuck, delete that
  file (`rm /opt/herdsman-calendar/ff_session.json`) to force a fresh fix on
  the next background refresh (within `CALENDAR_REFRESH_INTERVAL_LONG_S`/
  `_SHORT_S` depending on whether an event's coming up soon, or restart
  the service to force it immediately via its own startup fetch).
- forexfactory.com can change its HTML structure, so this service may need
  periodic selector updates. If events stop appearing, or impact levels all
  come back the same, the first things to check are `WIDGET_TABLE_CLASS`
  (`table.calendar__table`), the `calendar__*` cell classes in
  `parse_calendar()`, and the icon class suffixes in
  `IMPACT_ICON_SUFFIX_LEVELS` -- view-source `forexfactory.com/calendar`
  and compare.
- Impact levels came back `0`/`"holiday"` for every single event for a
  while, right after the Forex Factory switch -- not a markup change, a bug
  in this scraper's own code. `parse_impact_level()`'s BeautifulSoup class
  filter was written as `lambda c: c and any(cls.startswith(...) for cls in
  c)`, assuming `c` is always a list. BeautifulSoup actually invokes a
  class-filter callback once per individual class token *and* once with
  the full space-joined string -- so `c` is a plain string on every call,
  and iterating a string with `for cls in c` walks its *characters*, which
  never match a `.startswith("icon--ff-impact-")` check. Every call
  silently returned no match, `find()` returned `None`, and the level
  defaulted to 0 every time. Fixed with a plain substring check
  (`"icon--ff-impact-" in c`) instead, which correctly matches on the
  full-string call bs4 also makes. Verified directly against the live site
  afterward (real 1/2/3 distributions, not all-0).
- `/calendar`'s `day` field used to come back glued together with no space
  between the weekday and the month ("FriJul 31" instead of "Fri Jul 31"),
  reported from the ESP32 firmware's own display of it. Root cause: Forex
  Factory's `calendar__date` cell nests the weekday and "Mon DD" in
  separate sibling elements with no whitespace text node between them
  (`<span class="date">Fri<span>Jul 31</span></span>`), and
  `date_cell.get_text(strip=True)` concatenates text from separate
  elements with nothing in between -- confirmed directly against the live
  markup. Fixed with `get_text(separator=" ", strip=True)`, then
  `re.sub(r"\s+", " ", ...)` to collapse any resulting double spaces
  rather than assume the exact tag structure holds forever. Verified
  directly against the live site's actual markup (not just against the
  API's own output) before considering it fixed. The ESP32 firmware's
  `alert_manager.cpp` (`parse_event_timestamp()`) parses this same field
  to build countdown/refresh timestamps -- its weekday-skip logic changed
  from a hardcoded 3-char substring to "skip to the first space" to match,
  since a hardcoded skip would silently break the moment this field
  gained a space.
- A cache layer would be a good next step if the API is polled frequently.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
