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

## Filters: impact level + country

`importance` (which impact levels to show) and `countries` (which
countries) aren't fixed in code — they live in `filters.json`, created next
to `calendar_api.py` on first run with defaults of `{"importance": [2, 3],
"countries": [5]}` (medium/high impact, US only). This survives `update`
redeploys, since those only overwrite `calendar_api.py`/`requirements.txt`.

This makes each deployment independently configurable — useful since
different customers of the same product want different things (some want
low-impact events too, some want UK/China alongside the US, etc.) without
needing a per-customer server config or code change. The ESP32's Settings
screen is the intended way to change this day-to-day; the endpoints below
are what it calls.

```bash
# Current selection
curl http://<container-ip>:8080/filters

# All countries investing.com's widget supports, for building a picker UI
curl http://<container-ip>:8080/countries

# Update the selection (validated: importance must be 1/2/3, countries must
# be codes from GET /countries -- an invalid request leaves the previously
# saved filters untouched rather than partially applying)
curl -X POST http://<container-ip>:8080/filters \
  -H "Content-Type: application/json" \
  -d '{"importance": [1, 2, 3], "countries": [5, 4, 37]}'
```

Country codes (`GET /countries`) were scraped directly from investing.com's
own widget customization tool
(`investing.com/webmaster-tools/economic-calendar`, each country checkbox's
`id` attribute is its code) — not a third-party or guessed list. If a code
is ever suspected stale, that page is the source to re-check.

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
- `columns` must keep `exc_flags,exc_currency` even though nothing in the
  UI displays the flag icon — dropping them removes the currency-code cell
  from the HTML entirely, not just the visual flag. Confirmed directly by
  fetching the page with and without them.
- A cache layer would be a good next step if the API is polled frequently.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
