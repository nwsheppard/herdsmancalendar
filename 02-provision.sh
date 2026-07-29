#!/usr/bin/env bash
#
# Run this INSIDE the LXC container (as root) after 01-create-lxc.sh and
# copying this deploy/ folder in. Sets up Python, a venv, the service
# user, and enables the systemd service.
#
# Idempotent -- safe to re-run if something fails partway through.

set -euo pipefail

APP_DIR="/opt/herdsman-calendar"
SERVICE_USER="herdsman"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "== Updating system packages =="
apt-get update
apt-get upgrade -y

echo "== Installing Python and venv support =="
apt-get install -y python3 python3-venv python3-pip

echo "== Creating service user =="
if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
    useradd --system --home "$APP_DIR" --shell /usr/sbin/nologin "$SERVICE_USER"
fi

echo "== Setting up application directory =="
mkdir -p "$APP_DIR"
cp "$SCRIPT_DIR/calendar_api.py" "$APP_DIR/"
cp "$SCRIPT_DIR/requirements.txt" "$APP_DIR/"

echo "== Creating virtual environment =="
if [ ! -d "$APP_DIR/venv" ]; then
    python3 -m venv "$APP_DIR/venv"
fi
"$APP_DIR/venv/bin/pip" install --upgrade pip
"$APP_DIR/venv/bin/pip" install -r "$APP_DIR/requirements.txt"

echo "== Setting ownership =="
chown -R "$SERVICE_USER:$SERVICE_USER" "$APP_DIR"

echo "== Installing systemd service =="
cp "$SCRIPT_DIR/herdsman-calendar-api.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable herdsman-calendar-api
systemctl restart herdsman-calendar-api

echo "== Waiting for service to come up... =="
sleep 3

echo "== Checking health endpoint =="
if curl -sf http://localhost:8080/health > /dev/null; then
    echo "SUCCESS: service is up and responding."
    IP=$(hostname -I | awk '{print $1}')
    echo ""
    echo "Test it from another machine with:"
    echo "  curl http://$IP:8080/calendar?range=day"
    echo "  curl http://$IP:8080/calendar?range=week"
else
    echo "WARNING: health check failed. Check logs with:"
    echo "  journalctl -u herdsman-calendar-api -n 50 --no-pager"
fi
