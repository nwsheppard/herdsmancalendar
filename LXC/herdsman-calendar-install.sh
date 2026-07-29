#!/usr/bin/env bash

# Copyright (c) 2021-2026 nwsheppard
# Author: nwsheppard
# License: MIT | https://github.com/nwsheppard/herdsmancalendar/blob/main/LXC/LICENSE
# Source: https://github.com/nwsheppard/herdsmancalendar
#
# Fetched and executed INSIDE the LXC by build.func (via install.sh, which
# redirects the fetch to this file). Not meant to be run directly on the
# Proxmox host.

source /dev/stdin <<<"$FUNCTIONS_FILE_PATH"
color
verb_ip6
catch_errors
setting_up_container
network_check
update_os

msg_info "Installing Python"
$STD apt-get install -y python3 python3-venv python3-pip
msg_ok "Installed Python"

msg_info "Creating Service User"
if ! id -u herdsman >/dev/null 2>&1; then
  useradd --system --home /opt/herdsman-calendar --shell /usr/sbin/nologin herdsman
fi
msg_ok "Created Service User"

msg_info "Installing Herdsman Calendar API"
mkdir -p /opt/herdsman-calendar
curl -fsSL "https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC/calendar_api.py" -o /opt/herdsman-calendar/calendar_api.py
curl -fsSL "https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC/requirements.txt" -o /opt/herdsman-calendar/requirements.txt
if [[ ! -d /opt/herdsman-calendar/venv ]]; then
  $STD python3 -m venv /opt/herdsman-calendar/venv
fi
$STD /opt/herdsman-calendar/venv/bin/pip install --upgrade pip
$STD /opt/herdsman-calendar/venv/bin/pip install -r /opt/herdsman-calendar/requirements.txt
chown -R herdsman:herdsman /opt/herdsman-calendar
msg_ok "Installed Herdsman Calendar API"

msg_info "Creating Service"
cat <<EOF >/etc/systemd/system/herdsman-calendar-api.service
[Unit]
Description=Herdsman Trading Terminal - Economic Calendar API
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=herdsman
WorkingDirectory=/opt/herdsman-calendar
Environment=PATH=/opt/herdsman-calendar/venv/bin
ExecStart=/opt/herdsman-calendar/venv/bin/uvicorn calendar_api:app --host 0.0.0.0 --port 8080
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
systemctl enable -q --now herdsman-calendar-api
msg_ok "Created Service"

motd_ssh
customize
cleanup_lxc
