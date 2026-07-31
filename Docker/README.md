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

## Quick start (docker compose)

From the repo root:

```bash
docker compose -f Docker/docker-compose.yml up -d --build
```

That builds the image and starts the container, publishing port 8080 and
creating a named volume (`herdsman_data`) for `filters.json` to persist in
across rebuilds/recreations. Rebuilding after pulling a newer
`calendar_api.py` is the same command again (`--build` picks up the change).

## Quick start (plain `docker`, no compose)

Also from the repo root -- note the `-f`/context arguments, since the
Dockerfile isn't in the current directory:

```bash
docker build -f Docker/Dockerfile -t herdsman-calendar:latest .
docker run -d \
  --name herdsman-calendar \
  -p 8080:8080 \
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
