#!/usr/bin/env bash
source <(curl -fsSL https://raw.githubusercontent.com/community-scripts/ProxmoxVE/main/misc/build.func | sed \
    -e 's#community-scripts/ProxmoxVE/main/install/#nwsheppard/herdsmancalendar/main/LXC/#g' \
    -e 's#if whiptail --yesno "A newer template is available.*Do you want to download and use it instead?" 12 70; then#if true; then#')
# Copyright (c) 2021-2026 nwsheppard
# Author: nwsheppard
# License: MIT | https://github.com/nwsheppard/herdsmancalendar/blob/main/LXC/LICENSE
# Source: https://github.com/nwsheppard/herdsmancalendar
#
# This sources the real community-scripts/ProxmoxVE build.func to get the
# same interactive experience their scripts provide (Default/Advanced
# Install menu, storage + resource prompts, container creation). The first
# `sed` rewrites the one line in build.func that fetches the per-app install
# script -- that line is hardcoded to community-scripts' own repo, so it has
# to be redirected at fetch time for a private app like this one. The second
# `sed` forces the "a newer base template is available, download it?" prompt
# to always take the update path instead of asking, since we always want the
# current template. Everything else (core.func, error_handler.func,
# tools.func) loads unmodified straight from their repo, so this always
# tracks their latest helper behavior with nothing vendored here to fall out
# of date.

APP="Herdsman-Calendar"
var_tags="${var_tags:-finance}"
var_cpu="${var_cpu:-1}"
var_ram="${var_ram:-512}"
var_disk="${var_disk:-4}"
var_os="${var_os:-debian}"
var_version="${var_version:-12}"
var_unprivileged="${var_unprivileged:-1}"

header_info "$APP"
variables
color
catch_errors

function update_script() {
  header_info
  check_container_storage
  check_container_resources

  if [[ ! -d /opt/herdsman-calendar ]]; then
    msg_error "No ${APP} Installation Found!"
    exit
  fi

  msg_info "Updating ${APP}"
  systemctl stop herdsman-calendar-api
  curl -fsSL "https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC/calendar_api.py" -o /opt/herdsman-calendar/calendar_api.py
  curl -fsSL "https://raw.githubusercontent.com/nwsheppard/herdsmancalendar/main/LXC/requirements.txt" -o /opt/herdsman-calendar/requirements.txt
  $STD /opt/herdsman-calendar/venv/bin/pip install -r /opt/herdsman-calendar/requirements.txt
  systemctl start herdsman-calendar-api
  msg_ok "Updated Successfully!"
  exit
}

start
build_container
description

msg_ok "Completed Successfully!\n"
echo -e "${CREATING}${GN}${APP} setup has been successfully initialized!${CL}"
echo -e "${INFO}${YW}Access it using the following URLs:${CL}"
echo -e "${GATEWAY}${BGN}http://${IP}:8080/calendar?range=day${CL}"
echo -e "${GATEWAY}${BGN}http://${IP}:8080/calendar?range=week${CL}"
