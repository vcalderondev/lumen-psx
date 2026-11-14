#!/bin/sh
# Lumen session: frontend -> RetroArch -> back to the frontend, until the user
# exits the frontend (Select+Start).
#
# The frontend exits 10 after writing the launch request /tmp/lumen-launch
# (v2: "key=value" lines, see docs/settings-design.md 7.5; a v1 request is two
# lines, core and content).
#
# Optional $LUMEN_HOME/lumen.conf (sourced) can set:
#   RETROARCH            path to the retroarch binary (default: retroarch in PATH)
#   LUMEN_MAIN_SAVES     save folder of the "main" profile (empty: RetroArch decides)
#   LUMEN_MAIN_STATES    save-state folder of the "main" profile
#   LUMEN_CORE_OPT_DIR   folder holding RetroArch's <Core Name>/<Core Name>.opt
#   run_retroarch()      function(core, content, logfile) to launch a game; it must
#                        apply the extra config in $LUMEN_RA_EXTRA (RetroArch cfg lines)
LUMEN_HOME="${LUMEN_HOME:-$(cd "$(dirname "$0")" && pwd)}"
LOG="${LUMEN_HOME}/logs"
RETROARCH="${RETROARCH:-retroarch}"
LUMEN_MAIN_SAVES=""
LUMEN_MAIN_STATES=""
LUMEN_CORE_OPT_DIR=""
LUMEN_RA_EXTRA=/tmp/lumen-ra-extra.cfg
mkdir -p "${LOG}"
chmod +x "${LUMEN_HOME}/lumen" 2>/dev/null
[ -z "${XDG_RUNTIME_DIR}" ] && export XDG_RUNTIME_DIR=/run/user/0

slog() { echo "$*" >> "${LOG}/session.log"; }

# lumen_parse_launch FILE: sets L_CORE L_CONTENT L_SYSTEM L_TITLE L_PROFILE L_MODE
# L_DEMO_SECONDS L_GRAPHICS L_OPT_CORE_NAME; writes the opt.* pairs to /tmp/lumen-opt.kv
# and the ra.* pairs to /tmp/lumen-ra.kv ("key=value" lines). Fails without core/content.
lumen_parse_launch() {
  L_CORE=""; L_CONTENT=""; L_SYSTEM=""; L_TITLE=""; L_PROFILE=main; L_MODE=normal
  L_DEMO_SECONDS=60; L_GRAPHICS=""; L_OPT_CORE_NAME=""
  : > /tmp/lumen-opt.kv; : > /tmp/lumen-ra.kv
  [ -f "$1" ] || return 1
  first="$(sed -n 1p "$1")"
  case "$first" in
    *=*)
      while IFS= read -r line || [ -n "$line" ]; do
        key=${line%%=*}; val=${line#*=}
        case "$key" in
          core) L_CORE=$val ;;
          content) L_CONTENT=$val ;;
          system) L_SYSTEM=$val ;;
          title) L_TITLE=$val ;;
          profile) L_PROFILE=$val ;;
          mode) L_MODE=$val ;;
          demo_seconds) L_DEMO_SECONDS=$val ;;
          graphics) L_GRAPHICS=$val ;;
          opt_core_name) L_OPT_CORE_NAME=$val ;;
          opt.*) echo "${key#opt.}=${val}" >> /tmp/lumen-opt.kv ;;
          ra.*) echo "${key#ra.}=${val}" >> /tmp/lumen-ra.kv ;;
        esac
      done < "$1"
      ;;
    *)  # v1: core path, content path
      L_CORE=$first
      L_CONTENT="$(sed -n 2p "$1")"
      ;;
  esac
  case "$L_PROFILE" in *[!a-z0-9_-]*|"") L_PROFILE=main ;; esac
  case "$L_MODE" in demo) ;; *) L_MODE=normal ;; esac
  case "$L_DEMO_SECONDS" in *[!0-9]*|"") L_DEMO_SECONDS=60 ;; esac
  [ -n "$L_CORE" ] && [ -n "$L_CONTENT" ]
}

# lumen_profile_dirs: L_SAVES / L_STATES of profile L_PROFILE ("main" keeps the
# folders from lumen.conf; empty = whatever RetroArch's own config says)
lumen_profile_dirs() {
  if [ "$L_PROFILE" != main ] && [ ! -d "${LUMEN_HOME}/profiles/${L_PROFILE}" ]; then
    slog "profile ${L_PROFILE} missing, using main"
    L_PROFILE=main
  fi
  if [ "$L_PROFILE" = main ]; then
    L_SAVES="${LUMEN_MAIN_SAVES}"; L_STATES="${LUMEN_MAIN_STATES}"
  else
    L_SAVES="${LUMEN_HOME}/profiles/${L_PROFILE}/saves"
    L_STATES="${LUMEN_HOME}/profiles/${L_PROFILE}/states"
    mkdir -p "$L_SAVES" "$L_STATES"
  fi
}

# lumen_build_ra_extra: RetroArch cfg lines appended after its own config
lumen_build_ra_extra() {
  : > "${LUMEN_RA_EXTRA}"
  [ -n "$L_SAVES" ] && echo "savefile_directory = \"${L_SAVES}\"" >> "${LUMEN_RA_EXTRA}"
  [ -n "$L_STATES" ] && echo "savestate_directory = \"${L_STATES}\"" >> "${LUMEN_RA_EXTRA}"
  export LUMEN_RA_EXTRA
}

run_retroarch() {  # core content logfile
  "${RETROARCH}" --appendconfig "${LUMEN_RA_EXTRA}" -L "$1" "$2" > "$3" 2>&1
}
[ -f "${LUMEN_HOME}/lumen.conf" ] && . "${LUMEN_HOME}/lumen.conf"

( while true; do sleep 3; sync; done ) &
SYNCER=$!
echo "lumen session $(date)" > "${LOG}/session.log"
# profiles deleted in the frontend whose background removal did not finish
rm -rf "${LUMEN_HOME}"/profiles/.trash-* 2>/dev/null

while true; do
  rm -f /tmp/lumen-launch
  SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}" "${LUMEN_HOME}/lumen" \
    --home "${LUMEN_HOME}" --log "${LOG}/lumen.log"
  rc=$?
  slog "lumen exit=${rc}"
  [ "${rc}" = "10" ] || break
  lumen_parse_launch /tmp/lumen-launch || { slog "launch request missing or invalid"; break; }
  lumen_profile_dirs
  lumen_build_ra_extra
  slog "launch: ${L_CONTENT} (system ${L_SYSTEM:-?}, profile ${L_PROFILE}, saves ${L_SAVES:-default})"
  run_retroarch "${L_CORE}" "${L_CONTENT}" "${LOG}/retroarch.log"
  slog "retroarch exit=$?"
done

kill "${SYNCER}" 2>/dev/null
sync
