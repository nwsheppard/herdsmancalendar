# Herdsman Trading Terminal -- Economic Calendar API, containerized.
#
# Build context is the REPO ROOT, not this Docker/ folder -- see
# docker-compose.yml's `context: ..` / the `docker build -f Docker/Dockerfile .`
# instructions in Docker/README.md. This lets the image reuse LXC/calendar_api.py
# and LXC/requirements.txt directly instead of keeping a second copy here that
# could drift out of sync with the Proxmox LXC install -- there is exactly one
# copy of the app's source in this repo, regardless of which way it gets deployed.

FROM python:3.12-slim

WORKDIR /app

# Requirements copied (and installed) before the app source so this layer
# stays cached across ordinary code changes -- only rebuilds when
# requirements.txt itself changes, not on every calendar_api.py edit.
COPY LXC/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY LXC/calendar_api.py .

# FILTERS_DIR (see calendar_api.py) -- a dedicated directory, not the app's
# own /app, so /data can be volume-mounted as a whole directory for
# filters.json to persist across container recreations/image updates.
# Pre-created here (not left for the app to create on first save) so the
# VOLUME below -- and any bind mount a user points at it -- has something
# real to attach to from the first `docker run`.
ENV FILTERS_DIR=/data
RUN mkdir -p /data
VOLUME /data

EXPOSE 8080

# Runs as PID 1 directly (no systemd/supervisor -- standard practice in a
# container, where the orchestrator/`docker restart`/`restart: unless-stopped`
# already fill that role). Same uvicorn invocation as the LXC install's
# systemd unit, so behavior matches exactly between the two deployment paths.
CMD ["uvicorn", "calendar_api:app", "--host", "0.0.0.0", "--port", "8080"]
