# Herdsman Calendar API

A small FastAPI service that scrapes the investing.com economic calendar
widget and serves clean JSON for an ESP32-based terminal client.

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

## Filters: impact level, country, category, and columns

`importance` (which impact levels to show), `countries` (which countries),
`categories` (which investing.com event categories, e.g. Employment,
Inflation, Central Banks), and `columns` (which optional per-event fields --
Impact/Actual/Forecast/Previous -- to include) aren't fixed in code — they
live in `filters.json`, created next to `calendar_api.py` with defaults of
`{"importance": [2, 3], "countries": [5], "categories": ["_employment",
"_economicActivity", "_inflation", "_credit", "_centralBanks", "_Bonds"],
"columns": ["exc_importance", "exc_actual", "exc_forecast",
"exc_previous"]}` the moment the service starts (a FastAPI `lifespan` hook
calls `load_filters()` on startup) — not lazily on the first `/calendar` or
`/filters` request, which is surprising to find missing if you go looking
for it right after `systemctl start`/`update`. This file survives `update`
redeploys, since those only overwrite `calendar_api.py`/`requirements.txt`.
Loading an older `filters.json` that predates `categories`/`columns` falls
back to those same defaults for just the missing keys.

This makes each deployment independently configurable — useful since
different customers of the same product want different things (some want
low-impact events too, some want UK/China alongside the US, some don't care
about Forecast/Previous values, etc.) without needing a per-customer server
config or code change. The ESP32's Settings screen is the intended way to
change this day-to-day; the endpoints below are what it calls.

```bash
# Current selection
curl http://<container-ip>:8080/filters

# All countries investing.com's widget supports, for building a picker UI
curl http://<container-ip>:8080/countries

# All event categories investing.com's widget supports
curl http://<container-ip>:8080/categories

# All optional per-event columns the ESP32 can choose to show
curl http://<container-ip>:8080/columns

# Update the selection (validated: importance must be 1/2/3, countries must
# be codes from GET /countries, categories from GET /categories, columns
# from GET /columns -- an invalid request leaves the previously saved
# filters untouched rather than partially applying)
curl -X POST http://<container-ip>:8080/filters \
  -H "Content-Type: application/json" \
  -d '{"importance": [1, 2, 3], "countries": [5, 4, 37],
       "categories": ["_employment", "_inflation"], "columns": ["exc_actual"]}'
```

Country codes (`GET /countries`) and category codes (`GET /categories`)
were both scraped directly from investing.com's own widget customization
tool (`investing.com/webmaster-tools/economic-calendar`, each checkbox's
`id` attribute is its code) — not a third-party or guessed list. If a code
is ever suspected stale, that page is the source to re-check.

`columns` intentionally excludes `exc_flags`/`exc_currency` from the
choosable set -- `exc_currency` is always requested regardless of the
saved selection, because dropping it doesn't just blank the currency field:
investing.com's HTML omits the `flagCur` cell's closing `</td>` entirely,
so every later cell in that row (event/actual/forecast/previous) ends up
nested inside it instead of being a sibling, corrupting the whole row
(verified directly, not a guess). `exc_flags` is the flag icon, which
nothing here ever reads, so it's never requested at all.

## Maintenance notes

- investing.com can change its HTML structure, so this service may need
  periodic selector updates. This already happened once: as of 2026-07,
  the impact-level icon classes changed from `GrayFullBullish` to
  lowercase `grayFullBullishIcon`/`grayEmptyBullishIcon`, silently breaking
  impact detection (every event came back `impact_level: 0`/`"unknown"`).
  Fixed by reading the sentiment cell's `title` attribute ("High/Moderate/Low
  Volatility Expected") as the primary signal instead, with icon-counting
  as a fallback — semantic text is less likely to silently drift than a
  CSS class name. If impact detection breaks again, check `title` first.
- The `columns`/`exc_currency` requirement above (see "Filters" section) is
  the same kind of stale-selector risk — confirmed directly by fetching the
  page with and without `exc_currency`, not guessed.
- The `day` field came back `null` for every event for a while: the
  day-separator rows (e.g. "Monday, July 27, 2026") turned out to be a
  `<tr>` with no `class`/`id` of its own — the `theDay` class this scraper
  checked for actually lives on that row's single child `<td>`. On top of
  that, the row query itself (`table.find_all("tr", id=...)`) only ever
  selected rows whose `id` contains `eventRowId`, so the separator row was
  filtered out before the loop even got a chance to look at it — the
  `"theDay" in classes` check was dead code from the start, not something
  that broke later. Fixed by selecting every `<tr>` in the table and
  checking each row's shape instead of pre-filtering by `id`: a row
  containing a child `<td class="theDay">` sets the current day, anything
  else without `eventRowId` in its own `id` (the header row, and the hidden
  per-event `eventInfoNNN` detail rows) is skipped, and everything else is
  parsed as an event row as before.
- A cache layer would be a good next step if the API is polled frequently.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
