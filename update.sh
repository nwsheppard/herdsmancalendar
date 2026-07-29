#!/usr/bin/env bash
#
# Run this INSIDE the LXC container (as root) to redeploy updated code.
# Copy the new calendar_api.py / requirements.txt into this deploy/
# folder first (e.g. via pct push from the Proxmox host), then run this.

set -euo pipefail

APP_DIR="/opt/herdsman-calendar"
SERVICE_USER="herdsman"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "== Copying updated files =="
cp "$SCRIPT_DIR/calendar_api.py" "$APP_DIR/"
cp "$SCRIPT_DIR/requirements.txt" "$APP_DIR/"

echo "== Updating dependencies (in case requirements.txt changed) =="
"$APP_DIR/venv/bin/pip" install -r "$APP_DIR/requirements.txt"

echo "== Fixing ownership =="
chown -R "$SERVICE_USER:$SERVICE_USER" "$APP_DIR"

echo "== Restarting service =="
systemctl restart herdsman-calendar-api
sleep 2

if curl -sf http://localhost:8080/health > /dev/null; then
    echo "SUCCESS: service restarted and healthy."
else
    echo "WARNING: health check failed after update. Check logs:"
    echo "  journalctl -u herdsman-calendar-api -n 50 --no-pager"
fi
