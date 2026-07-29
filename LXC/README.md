# Herdsman Calendar API

A small FastAPI service that scrapes the investing.com economic calendar
widget and serves clean JSON for an ESP32-based terminal client.

## What this repository contains

- `calendar_api.py` — the scraper and API service
- `requirements.txt` — Python dependencies
- `herdsman-calendar-api.service` — systemd service definition
- `01-create-lxc.sh` — creates the Proxmox LXC container
- `02-provision.sh` — provisions the container and installs the app
- `update.sh` — redeploys updated application files
- `install.sh` — repository-ready entrypoint for one-shot deployment

## Quick install

Once this folder is published to a GitHub repository, you can install it directly on a Proxmox host with:

```bash
curl -fsSL https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC/install.sh | REPO_BASE_URL=https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC bash
```

Optional overrides:

```bash
CTID=200 HOSTNAME=herdsman-calendar CORES=2 MEMORY=1024 SWAP=1024 REPO_BASE_URL=https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC bash
```

## Manual flow

1. Review the defaults in `01-create-lxc.sh` and adjust storage/template settings if needed.
2. Run the container creator on the Proxmox host.
3. Let the installer copy the deployment assets into the container and provision the service.
4. Test the API from another machine:

```bash
curl http://<container-ip>/calendar?range=day
curl http://<container-ip>/calendar?range=week
```

## Maintenance notes

- investing.com can change its HTML structure, so this service may need periodic selector updates.
- The current `countries` and `importance` values are simple defaults and may be adjusted later.
- A cache layer would be a good next step if the API is polled frequently.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
