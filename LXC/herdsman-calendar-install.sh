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
# FLARESOLVERR_URL: required for calendar_api.py to fetch anything at all
# (Forex Factory sits behind a Cloudflare JS challenge as of 2026-08 -- see
# calendar_api.py's module docstring). Picked up here *if* it happens to
# already be set in this script's environment (e.g. exported on the
# Proxmox host before running install.sh, if build.func forwards it
# through to this container-side script) -- not guaranteed, so this isn't
# relied on as the primary way to set it. If it's empty, the service still
# gets created and started (so the rest of this install completes
# normally), just without that line -- the clear post-install message
# below covers adding it either way, since there's no way to prompt
# interactively this deep into a piped `curl | bash` execution without
# risking breaking the whole non-interactive install flow.
FLARESOLVERR_ENV_LINE=""
if [[ -n "${FLARESOLVERR_URL:-}" ]]; then
  FLARESOLVERR_ENV_LINE="Environment=FLARESOLVERR_URL=${FLARESOLVERR_URL}"
fi
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
${FLARESOLVERR_ENV_LINE}
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

if [[ -z "${FLARESOLVERR_URL:-}" ]]; then
  echo -e "${YW}NOTE: FLARESOLVERR_URL isn't set -- the calendar API can't fetch"
  echo -e "anything without it (Forex Factory requires solving a Cloudflare"
  echo -e "challenge first; see LXC/README.md). Set it up:${CL}"
  echo -e "  pct exec <ctid> -- systemctl edit herdsman-calendar-api"
  echo -e "  # add under [Service]:"
  echo -e "  #   Environment=FLARESOLVERR_URL=http://<your-flaresolverr-host>:8191"
  echo -e "  pct exec <ctid> -- systemctl restart herdsman-calendar-api"
fi
