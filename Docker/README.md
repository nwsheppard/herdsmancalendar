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
  named volume for persisting `filters.json`.
- `Dockerfile.dockerignore` -- trims the build context (colocated with the
  Dockerfile by name so Docker finds it regardless of context location --
  see its own comment if your Docker version is old enough that this
  doesn't apply).
- `.env.example` -- template for the one required setting
  (`FLARESOLVERR_URL`, see below). Copy to `.env` (gitignored) and fill in
  your own value -- `docker-compose.yml` reads it automatically.

## Before you start: FlareSolverr is required

Forex Factory sits behind a Cloudflare JS challenge as of 2026-08 (see
`calendar_api.py`'s module docstring) -- this service can't fetch the
calendar at all without a [FlareSolverr](https://github.com/FlareSolverr/FlareSolverr)
instance to route the request through. FlareSolverr isn't bundled into
this image (it needs a real headless browser, a much heavier dependency
than this service's own footprint) -- run it separately (its own
container is the usual way; a one-liner is in its own README) somewhere
reachable from wherever this container ends up, then:

```bash
cp Docker/.env.example Docker/.env
# edit Docker/.env, set FLARESOLVERR_URL to your instance's address
```

Without this, every `/calendar` request fails with a clear error
naming exactly what's missing (`FLARESOLVERR_URL is not configured...`),
not a confusing timeout or an unrelated-looking failure.

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
Dockerfile isn't in the current directory. No `.env` file here (that's a
compose-specific convenience) -- pass `FLARESOLVERR_URL` directly:

```bash
docker build -f Docker/Dockerfile -t herdsman-calendar:latest .
docker run -d \
  --name herdsman-calendar \
  -p 8080:8080 \
  -e FLARESOLVERR_URL=http://192.168.1.50:8191 \
  -v herdsman_data:/data \
  --restart unless-stopped \
  herdsman-calendar:latest
```

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
  -e FLARESOLVERR_URL=http://192.168.1.50:8191 \
  -v herdsman_data:/data \
  --restart unless-stopped \
  <your-dockerhub-username>/herdsman-calendar:latest
```

## Testing the API

```bash
curl http://localhost:8080/health
curl http://localhost:8080/calendar?range=day
curl http://localhost:8080/calendar?range=week
```

(Swap `localhost` for the Docker host's actual address if you're testing
from somewhere else on the network -- the ESP32 firmware will need that
same address in Settings -> Calendar Server.)

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

If `/calendar` starts failing outright (not just missing/wrong data, but
every request erroring), check FlareSolverr itself before assuming
`calendar_api.py` broke: is it still running, is `FLARESOLVERR_URL` in
`Docker/.env` still correct, and can it still solve Forex Factory's
challenge right now (Cloudflare's own challenge mechanics change too,
independent of anything in this repo). `docker logs -f herdsman-calendar`
surfaces the specific error either way -- a missing/wrong
`FLARESOLVERR_URL` and a FlareSolverr-side failure look different in the
log (see `calendar_api.py`'s `fetch_calendar_html()`).
