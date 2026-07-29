#!/usr/bin/env bash
#
# Repository-ready Proxmox installer for the Herdsman Calendar API.
#
# Example install:
#   curl -fsSL https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC/install.sh | bash
#
# Optional environment overrides:
#   CTID=200 HOSTNAME=herdsman-calendar REPO_BASE_URL=https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC bash
#
# The script downloads the deployment assets from REPO_BASE_URL and then
# creates the LXC, copies the files in, and provisions the service.
#

set -euo pipefail

CTID="${CTID:-200}"
HOSTNAME="${HOSTNAME:-herdsman-calendar}"
REPO_BASE_URL="${REPO_BASE_URL:-https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC}"
TEMPLATE_STORAGE="${TEMPLATE_STORAGE:-local}"
CONTAINER_STORAGE="${CONTAINER_STORAGE:-local-lvm}"
TEMPLATE="${TEMPLATE:-debian-12-standard_12.7-1_amd64.tar.zst}"
CORES="${CORES:-1}"
MEMORY="${MEMORY:-512}"
SWAP="${SWAP:-512}"
ROOTFS_SIZE="${ROOTFS_SIZE:-4}"
BRIDGE="${BRIDGE:-vmbr0}"
WORK_DIR="${WORK_DIR:-$(mktemp -d)}"

trap 'rm -rf "$WORK_DIR"' EXIT

if ! command -v pct >/dev/null 2>&1; then
    echo "Error: pct is not available on this Proxmox host." >&2
    exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "Error: curl is required to download the deployment files." >&2
    exit 1
fi

if [[ "$REPO_BASE_URL" != http* ]]; then
    echo "Error: REPO_BASE_URL must be a valid HTTP(S) URL." >&2
    exit 1
fi

for asset in calendar_api.py requirements.txt herdsman-calendar-api.service 02-provision.sh update.sh; do
    echo "== Downloading $asset =="
    curl -fsSL "$REPO_BASE_URL/$asset" -o "$WORK_DIR/$asset"
done

echo "== Checking for template $TEMPLATE =="
if ! pveam list "$TEMPLATE_STORAGE" | grep -q "$TEMPLATE"; then
    echo "Template not found locally; downloading..."
    pveam update
    pveam download "$TEMPLATE_STORAGE" "$TEMPLATE"
fi

echo "== Creating LXC $CTID ($HOSTNAME) =="
pct create "$CTID" "${TEMPLATE_STORAGE}:vztmpl/${TEMPLATE}" \
    --hostname "$HOSTNAME" \
    --cores "$CORES" \
    --memory "$MEMORY" \
    --swap "$SWAP" \
    --rootfs "${CONTAINER_STORAGE}:${ROOTFS_SIZE}" \
    --net0 "name=eth0,bridge=$BRIDGE,ip=dhcp" \
    --unprivileged 1 \
    --features nesting=1 \
    --onboot 1 \
    --start 1

echo "== Waiting for the container to become reachable =="
sleep 8

echo "== Preparing /root/deploy inside the container =="
pct exec "$CTID" -- mkdir -p /root/deploy

echo "== Copying deployment files into the container =="
pct push "$CTID" "$WORK_DIR/calendar_api.py" /root/deploy/calendar_api.py
pct push "$CTID" "$WORK_DIR/requirements.txt" /root/deploy/requirements.txt
pct push "$CTID" "$WORK_DIR/herdsman-calendar-api.service" /root/deploy/herdsman-calendar-api.service
pct push "$CTID" "$WORK_DIR/02-provision.sh" /root/deploy/02-provision.sh
pct push "$CTID" "$WORK_DIR/update.sh" /root/deploy/update.sh

echo "== Running the container provisioning script =="
pct exec "$CTID" -- bash /root/deploy/02-provision.sh

echo "== Done =="
echo "The service should soon be reachable at http://<container-ip>:8080"
