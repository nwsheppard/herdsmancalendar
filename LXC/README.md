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

## Testing the API

```bash
curl http://<container-ip>:8080/health
curl http://<container-ip>:8080/calendar?range=day
curl http://<container-ip>:8080/calendar?range=week
```

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
request). That turned out to be unnecessary: Forex Factory's calendar event
data is server-rendered in the initial HTML response, and the site doesn't
sit behind Cloudflare bot management the way investing.com did -- a plain
`requests` call gets a normal 200 (verified directly). So this stayed the
same lightweight `curl_cffi` (kept as cheap insurance rather than because
it's currently required) `+ BeautifulSoup` service it always was, just
pointed at a different site.

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

## Maintenance notes

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
- A cache layer would be a good next step if the API is polled frequently.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
