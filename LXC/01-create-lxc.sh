#!/usr/bin/env bash
#
# Run this ON THE PROXMOX HOST (not inside a container) to create the LXC.
# After it finishes, copy the rest of this deploy/ folder into the new
# container and run 02-provision.sh from inside it.
#
# Usage: ./01-create-lxc.sh [CTID]
#   CTID defaults to 200 if not given -- change if that ID is taken.

set -euo pipefail

CTID="${1:-200}"
HOSTNAME="herdsman-calendar"
TEMPLATE_STORAGE="local"                 # where templates live on your host
CONTAINER_STORAGE="local-lvm"            # where the container's disk goes -- adjust to your storage pool
TEMPLATE="debian-12-standard_12.7-1_amd64.tar.zst"  # adjust to whatever's current in your template list

echo "== Checking for the Debian 12 template =="
if ! pveam list "$TEMPLATE_STORAGE" | grep -q "$TEMPLATE"; then
    echo "Template not found locally, downloading..."
    pveam update
    pveam download "$TEMPLATE_STORAGE" "$TEMPLATE"
fi

echo "== Creating LXC $CTID ($HOSTNAME) =="
pct create "$CTID" "${TEMPLATE_STORAGE}:vztmpl/${TEMPLATE}" \
    --hostname "$HOSTNAME" \
    --cores 1 \
    --memory 512 \
    --swap 512 \
    --rootfs "${CONTAINER_STORAGE}:4" \
    --net0 name=eth0,bridge=vmbr0,ip=dhcp \
    --unprivileged 1 \
    --features nesting=1 \
    --onboot 1 \
    --start 1

echo "== Waiting for network... =="
sleep 8

echo "== Done. Container $CTID is running. =="
echo "Next steps:"
echo "  1. pct push $CTID calendar_api.py /root/calendar_api.py"
echo "  2. pct push $CTID requirements.txt /root/requirements.txt"
echo "  3. pct push $CTID herdsman-calendar-api.service /root/herdsman-calendar-api.service"
echo "  4. pct push $CTID 02-provision.sh /root/02-provision.sh"
echo "  5. pct exec $CTID -- bash /root/02-provision.sh"
echo ""
echo "Or just copy the whole deploy/ folder in at once:"
echo "  pct push $CTID <local-path>/deploy /root/deploy -r"
echo "  pct exec $CTID -- bash /root/deploy/02-provision.sh"
