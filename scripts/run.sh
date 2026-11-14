#!/bin/sh
# Lumen session: frontend -> RetroArch -> back to the frontend, until the user
# exits the frontend (Select+Start).
#
# Optional $LUMEN_HOME/lumen.conf (sourced) can override:
#   RETROARCH            path to the retroarch binary (default: retroarch in PATH)
#   run_retroarch()      function(core, content, logfile) to launch a game
LUMEN_HOME="${LUMEN_HOME:-$(cd "$(dirname "$0")" && pwd)}"
LOG="${LUMEN_HOME}/logs"
RETROARCH="${RETROARCH:-retroarch}"
mkdir -p "${LOG}"
chmod +x "${LUMEN_HOME}/lumen" 2>/dev/null
[ -z "${XDG_RUNTIME_DIR}" ] && export XDG_RUNTIME_DIR=/run/user/0

run_retroarch() {  # core content logfile
  "${RETROARCH}" -L "$1" "$2" > "$3" 2>&1
}
[ -f "${LUMEN_HOME}/lumen.conf" ] && . "${LUMEN_HOME}/lumen.conf"

( while true; do sleep 3; sync; done ) &
SYNCER=$!
echo "lumen session $(date)" > "${LOG}/session.log"

while true; do
  rm -f /tmp/lumen-launch
  SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}" "${LUMEN_HOME}/lumen" \
    --home "${LUMEN_HOME}" --log "${LOG}/lumen.log"
  rc=$?
  echo "lumen exit=${rc}" >> "${LOG}/session.log"
  [ "${rc}" = "10" ] && [ -f /tmp/lumen-launch ] || break
  CORE="$(sed -n 1p /tmp/lumen-launch)"
  GAME="$(sed -n 2p /tmp/lumen-launch)"
  echo "launch: ${GAME}" >> "${LOG}/session.log"
  run_retroarch "${CORE}" "${GAME}" "${LOG}/retroarch.log"
  echo "retroarch exit=$?" >> "${LOG}/session.log"
done

kill "${SYNCER}" 2>/dev/null
sync
