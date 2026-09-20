# Herdsman Calendar API -- Docker

The same FastAPI service documented in [`LXC/README.md`](../LXC/README.md)
(scrapes the Forex Factory economic calendar, serves clean JSON for the
ESP32 terminal client), packaged as a container for anyone who doesn't run
Proxmox. There is exactly one copy of the application source in this repo
(`LXC/calendar_api.py` + `LXC/requirements.txt`) -- this folder only adds
the container-specific plumbing (`Dockerfile`, `docker-compose.yml`,
`.dockerignore`) on top of it, so the two deployment paths can never drift
out of sync with each other.

## What this folder contains

- `Dockerfile` -- builds the image. Build context is the **repo root**,
  not this folder (see its own header comment for why) -- every command
  below accounts for that.
- `docker-compose.yml` -- the easiest way to build + run it, including a
  named volume for persisting `filters.json` and `ff_session.json` (the
  persisted Forex Factory session cookies that pin the calendar's timezone
  -- see `calendar_api.py`'s module docstring).
- `Dockerfile.dockerignore` -- trims the build context (colocated with the
  Dockerfile by name so Docker finds it regardless of context location --
  see its own comment if your Docker version is old enough that this
  doesn't apply).
- `.env.example` -- template for the one optional setting
  (`FLARESOLVERR_URL`, see below). Only needed if you want to point at a
  challenge-solver running elsewhere instead of the bundled one -- copy to
  `.env` (gitignored) and fill in your own value if so;
  `docker-compose.yml` reads it automatically.

## Before you start: a Cloudflare-challenge solver, bundled by default

Forex Factory sits behind a Cloudflare Turnstile/Managed Challenge as of
2026-08 (see `calendar_api.py`'s module docstring) -- this service can't
fetch the calendar at all without something that can solve that challenge
and hand back the resulting HTML. `docker-compose.yml` bundles
[Byparr](https://github.com/ThePhaseless/Byparr) as its own service for
exactly this, wired up automatically -- **`docker compose up` alone is
enough, nothing to configure first.**

Byparr, not [FlareSolverr](https://github.com/FlareSolverr/FlareSolverr),
was chosen deliberately: both speak the same `/v1` API and default port
(8191), so either works here with zero code changes, but FlareSolverr's
Selenium/undetected-chromedriver approach reliably failed to clear Forex
Factory's current Turnstile challenge in direct testing (every solve
attempt timed out against FlareSolverr's own 60s budget), while Byparr's
Camoufox-backed solver cleared it -- see `LXC/README.md`'s own writeup of
that investigation. Byparr isn't baked into `herdsman-calendar`'s own image
(same reasoning FlareSolverr always had here: a real browser is a much
heavier dependency than this service's own footprint) -- it's a sibling
container in the same compose file instead, reachable internally at
`http://byparr:8191`, nothing published to the host.

Already run your own FlareSolverr or Byparr elsewhere (e.g. for other *arr
apps)? Point at that instead of the bundled one:

```bash
cp Docker/.env.example Docker/.env
# uncomment FLARESOLVERR_URL in Docker/.env, set it to your instance's address
```

Whichever solver ends up unreachable or misconfigured, the container still
starts, but its background calendar refresh (see "How /calendar stays
fast" below) can never actually succeed -- `docker logs -f herdsman-calendar`
shows a clear `Background refresh of /calendar?range=... failed: ...` error
on every attempt (check `docker logs -f herdsman-byparr` too, if you're
using the bundled one -- a Camoufox crash from too little shared memory is
the most common cause, see `docker-compose.yml`'s own `shm_size` comment).
`/calendar` itself won't show that error directly; it'll either serve
nothing yet (a 503 right after a fresh start) or keep serving increasingly
stale data if it had fetched successfully before the solver became
unreachable. Check the logs, not the client response, if you're not sure
what's wrong.

## Quick start (docker compose)

From the repo root (after the `.env` setup above):

```bash
docker compose -f Docker/docker-compose.yml up -d --build
```

That builds the image and starts the container, publishing port 8080 and
creating a named volume (`herdsman_data`) for `filters.json` to persist in
across rebuilds/recreations. Rebuilding after pulling a newer
`calendar_api.py` is the same command again (`--build` picks up the change).

## Quick start (plain `docker`, no compose)

Also from the repo root -- note the `-f`/context arguments, since the
Dockerfile isn't in the current directory. `docker-compose.yml`'s bundled
Byparr service is a compose-specific convenience -- plain `docker run`
needs its own container for it, run once and reused across
`herdsman-calendar` restarts/rebuilds:

```bash
docker run -d \
  --name herdsman-byparr \
  --shm-size 512m \
  --restart unless-stopped \
  ghcr.io/thephaseless/byparr:latest

docker build -f Docker/Dockerfile -t herdsman-calendar:latest .
docker run -d \
  --name herdsman-calendar \
  -p 8080:8080 \
  -e FLARESOLVERR_URL=http://herdsman-byparr:8191 \
  --link herdsman-byparr \
  -v herdsman_data:/data \
  --restart unless-stopped \
  herdsman-calendar:latest
```

(`--link` is legacy Docker, but it's the simplest way for one plain
`docker run` container to resolve another by name without hand-rolling a
user-defined network -- `docker-compose.yml` gets this for free from
compose's own default network, which is the main reason it's the
recommended path over this one. Already running FlareSolverr or Byparr
elsewhere instead? Skip the first `docker run` above and point
`FLARESOLVERR_URL` at that instance's address directly.)

`-v herdsman_data:/data` is the same volume/mount-point the compose file
uses -- a whole directory, not `filters.json` itself. See the Dockerfile's
`FILTERS_DIR` comment: Docker creates a plain directory (not a file) at a
single-file mount target that doesn't already exist as a file in the
image, which would break the app's first attempt to *save* a filters
change with nothing valid there yet. Mounting the whole directory avoids
that entirely.

## Using a published image instead of building locally

Once Docker Hub publishing is set up (see the repo root's instructions --
not committed to this folder, since it's a one-time GitHub-side setup, not
part of what this image needs to run), replace the `build:` block in
`docker-compose.yml` with `image: <your-dockerhub-username>/herdsman-calendar:latest`,
or run directly:

```bash
docker run -d \
  --name herdsman-calendar \
  -p 8080:8080 \
  -e FLARESOLVERR_URL=http://herdsman-byparr:8191 \
  --link herdsman-byparr \
  -v herdsman_data:/data \
  --restart unless-stopped \
  <your-dockerhub-username>/herdsman-calendar:latest
```

(Assumes the same `herdsman-byparr` container from "Quick start (plain
docker, no compose)" above is already running -- swap `FLARESOLVERR_URL`
for wherever your own solver actually lives if you're not using it.)

## Testing the API

```bash
curl http://localhost:8080/health
curl http://localhost:8080/calendar?range=day
curl http://localhost:8080/calendar?range=week
curl "http://localhost:8080/calendar/wait?range=day&since=-1"   # instant -- since=-1 always "changed"
curl "http://localhost:8080/calendar/wait?range=day&since=1"    # blocks up to CALENDAR_LONG_POLL_TIMEOUT_S
```

(Swap `localhost` for the Docker host's actual address if you're testing
from somewhere else on the network -- the ESP32 firmware will need that
same address in Settings -> Calendar Server.)

## How /calendar stays fast

`/calendar` never talks to FlareSolverr itself -- a background thread
refreshes both `day` and `week` caches on its own schedule
(`calendar_api.py`), and the endpoint just reads whatever's currently
cached. Poll it as often as you like; nothing a client does ever
triggers a FlareSolverr round trip or waits on one.

That schedule isn't a single fixed interval: `CALENDAR_REFRESH_INTERVAL_LONG_S`
(3 hours) is the steady-state cadence, switching to the much shorter
`CALENDAR_REFRESH_INTERVAL_SHORT_S` (1 minute) whenever a cached event's
scheduled time is within `CALENDAR_EVENT_PROXIMITY_WINDOW_S` (30 minutes)
of right now -- a flat cadence around the clock was more load against
Forex Factory than the data (which only actually changes around events'
own scheduled times) justifies, but a long fixed interval alone would've
silently broken the ESP32's own post-event refresh
(`alert_manager_tick()`, `alert_manager.cpp`), which only finds anything
new if this cache happened to have refreshed recently enough to have it.

A real bug lived in how that switch got decided, not just how tight it
was: the refresh loop used to sleep through an *entire* decided interval
in one blocking wait before checking anything again, which silently broke
the first long-to-short transition for almost every event -- a 3-hour
sleep meant an event could enter and exit its 30-minute proximity window
with nobody ever noticing. Fixed so the loop never sleeps longer than
`CALENDAR_REFRESH_INTERVAL_SHORT_S` at a stretch regardless of which
interval is in effect, catching that transition within about a minute
instead of missing it for up to 3 hours. See `LXC/README.md`'s own
writeup for the full story -- reported directly from a real event whose
actual value never landed in this cache at all.

One consequence: `docker compose up`/`docker run` won't report the
container healthy until the first fetch completes (up to ~65s x2 in the
worst case, both ranges needing a timezone self-heal) -- `lifespan()`
does that fetch synchronously before the service starts accepting
requests, so `/calendar` has real data from the moment it's reachable
rather than a guaranteed empty window right after every start. A
background refresh that fails afterward just logs and keeps serving the
last good cached copy -- the only client-visible error is a 503 in the
narrow window right after a fresh start, before the first refresh has
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

This is what the ESP32 firmware actually uses now -- no WebSockets, no
persistent connection to manage, just the same plain HTTP GET repeated in
a loop, one request immediately following the last. A change on the
backend reaches the screen within moments of the background thread
picking it up, not up to a full poll interval later. A version only
bumps when the underlying events actually change, not on every
background refresh cycle -- and not on a filter change either (same
underlying events, just filtered differently), which is why the ESP32
always issues an immediate `since=-1` request right after a Settings
change rather than waiting for its current long-poll to resolve.

## Updating

```bash
docker compose -f Docker/docker-compose.yml up -d --build
```

Pulls in whatever's currently in `LXC/calendar_api.py`/`requirements.txt`
and rebuilds -- `filters.json` survives in the named volume regardless,
same as the LXC install's `update` command surviving redeploys for the
same reason (a persisted file, not baked into the image/container).

## Logs

```bash
docker logs -f herdsman-calendar
```

Logging goes straight to stdout/stderr (`logging.basicConfig()` in
`calendar_api.py`, no file handler) specifically because that's what
`docker logs` (and any log aggregator sitting in front of it) expects --
this didn't need to change for the container at all, unlike `filters.json`.

## Maintenance notes

See [`LXC/README.md`](../LXC/README.md)'s own Maintenance notes section --
Forex Factory scraper selector drift, past bugs found/fixed, etc. all
apply identically here, since it's the same `calendar_api.py` either way.

If `/calendar` is serving stale or empty data (see "How /calendar stays
fast" above -- a background refresh failure never surfaces as a client
error), check `docker logs -f herdsman-calendar` for
`Background refresh of /calendar?range=... failed: ...` lines, then check
the solver itself before assuming `calendar_api.py` broke: is
`herdsman-byparr` (or your own FlareSolverr/Byparr, if you're pointing at
one instead) still running (`docker logs -f herdsman-byparr`), is
`FLARESOLVERR_URL` still correct (unset in `Docker/.env` means it's using
the bundled `byparr` service by default -- see "Before you start" above),
and can it still solve Forex Factory's challenge right now (Cloudflare's
own challenge mechanics change too, independent of anything in this repo
-- this is exactly what broke FlareSolverr specifically in 2026-08, see
`LXC/README.md`).

If event times look off by a fixed offset instead (commonly "+1 hour"
during EDT), that's Forex Factory's IP-geolocated timezone default, not a
bug in this scraper -- `fetch_calendar_html()` self-heals it automatically
via `ff_session.json` in the `herdsman_data` volume. If it's stuck, remove
just that file from the volume (or `docker volume rm herdsman_data` to
reset everything, including `filters.json`) to force a fresh fix on the
next background refresh (within `CALENDAR_REFRESH_INTERVAL_LONG_S`/
`_SHORT_S` depending on whether an event's coming up soon, or restart
the container to force it immediately via its own startup fetch).
