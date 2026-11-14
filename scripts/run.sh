#!/bin/sh
# Lumen session: frontend -> RetroArch -> back to the frontend, until the user
# exits the frontend (Select+Start).
#
# The frontend exits 10 after writing the launch request /tmp/lumen-launch
# (v2: "key=value" lines, see docs/settings-design.md 7.5; a v1 request is two
# lines, core and content). It exits 11 for an attract-mode demo: the game runs as
# an ephemeral session (saves in /tmp, removed afterwards) for demo_seconds or until
# a button is pressed (lumen-buttons --demo-watch), then the frontend comes back
# with --attract (the demo ended) or --focus-demo (the player pressed a button).
#
# Optional $LUMEN_HOME/lumen.conf (sourced) can set:
#   RETROARCH            path to the retroarch binary (default: retroarch in PATH)
#   LUMEN_MAIN_SAVES     save folder of the "main" profile (empty: RetroArch decides)
#   LUMEN_MAIN_STATES    save-state folder of the "main" profile
#   LUMEN_CORE_OPT_DIR   folder holding RetroArch's <Core Name>/<Core Name>.opt
#                        (graphics presets are merged there; empty: not merged)
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

# lumen_kv_get KEY KVFILE: KV_VAL = value of KEY in a "key=value" file (last wins)
lumen_kv_get() {
  KV_VAL=""; kv_found=1
  while IFS= read -r kvl || [ -n "$kvl" ]; do
    [ "${kvl%%=*}" = "$1" ] && { KV_VAL=${kvl#*=}; kv_found=0; }
  done < "$2"
  return $kv_found
}

# lumen_merge_opt OPTFILE KVFILE: write the graphics preset's per-core options
# (opt.* pairs of the request) into RetroArch's <Core Name>.opt: declared keys are
# replaced, missing ones appended, every other line is kept. The first merge keeps
# the original file as <file>.lumen-bak. Sets L_OPT_FILE when the file was written.
lumen_merge_opt() {
  L_OPT_FILE=""
  [ -n "${LUMEN_CORE_OPT_DIR}" ] && [ -n "${L_OPT_CORE_NAME}" ] && [ -s "$2" ] || return 0
  mkdir -p "$(dirname "$1")" || return 1
  [ -f "$1" ] && [ ! -f "$1.lumen-bak" ] && cp "$1" "$1.lumen-bak"
  : > "$1.tmp" || return 1
  seen="|"
  if [ -f "$1" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      case "$line" in
        *" ="*)
          key=${line%% =*}
          if lumen_kv_get "$key" "$2"; then
            echo "${key} = \"${KV_VAL}\"" >> "$1.tmp"; seen="${seen}${key}|"; continue
          fi ;;
      esac
      echo "$line" >> "$1.tmp"
    done < "$1"
  fi
  while IFS= read -r kvl || [ -n "$kvl" ]; do
    key=${kvl%%=*}
    case "$seen" in *"|${key}|"*) continue ;; esac
    lumen_kv_get "$key" "$2"
    echo "${key} = \"${KV_VAL}\"" >> "$1.tmp"; seen="${seen}${key}|"
  done < "$2"
  mv -f "$1.tmp" "$1" || return 1
  sync
  L_OPT_FILE=$1
}

# lumen_build_ra_extra: RetroArch cfg lines that must win over its own config
# (profile save folders, the graphics preset's ra.* overrides, demo settings); it is
# handed over with --appendconfig. RetroArch keeps the FIRST copy of a key within one
# file, so these lines must never just be concatenated after its retroarch.cfg.
# The merged .opt needs no line here: RetroArch reads its per-core file by itself.
lumen_build_ra_extra() {
  : > "${LUMEN_RA_EXTRA}"
  [ -n "$L_SAVES" ] && echo "savefile_directory = \"${L_SAVES}\"" >> "${LUMEN_RA_EXTRA}"
  [ -n "$L_STATES" ] && echo "savestate_directory = \"${L_STATES}\"" >> "${LUMEN_RA_EXTRA}"
  if [ -f /tmp/lumen-ra.kv ]; then
    while IFS= read -r kvl || [ -n "$kvl" ]; do
      [ -n "$kvl" ] && echo "${kvl%%=*} = \"${kvl#*=}\"" >> "${LUMEN_RA_EXTRA}"
    done < /tmp/lumen-ra.kv
  fi
  if [ "$L_MODE" = demo ]; then   # ephemeral: nothing about the demo is kept
    for k in savestate_auto_save savestate_auto_load history_list_enable content_runtime_log \
             content_runtime_log_aggregate sort_savefiles_enable sort_savestates_enable \
             savefiles_in_content_dir savestates_in_content_dir; do
      echo "$k = \"false\"" >> "${LUMEN_RA_EXTRA}"
    done
  fi
  export LUMEN_RA_EXTRA
}

# lumen_run_demo: run the request as an ephemeral demo session; sets ARGS for the
# next frontend start (--focus-demo after a button press, --attract otherwise)
lumen_run_demo() {
  rm -rf /tmp/lumen-demo; mkdir -p /tmp/lumen-demo/saves /tmp/lumen-demo/states
  L_SAVES=/tmp/lumen-demo/saves; L_STATES=/tmp/lumen-demo/states
  lumen_build_ra_extra
  slog "demo: ${L_CONTENT} (system ${L_SYSTEM:-?}, ${L_DEMO_SECONDS} s, graphics ${L_GRAPHICS:-default})"
  run_retroarch "${L_CORE}" "${L_CONTENT}" "${LOG}/retroarch-demo.log" &
  RA_JOB=$!
  T=""
  if [ -x "${LUMEN_HOME}/lumen-buttons" ]; then
    "${LUMEN_HOME}/lumen-buttons" --demo-watch "${L_DEMO_SECONDS}" --pid "${RA_JOB}" --log "${LOG}/buttons.log"
    w=$?
  else   # fallback: timeout only
    ( sleep "${L_DEMO_SECONDS}"; killall -TERM retroarch 2>/dev/null; sleep 3; kill -TERM "${RA_JOB}" 2>/dev/null ) &
    T=$!; w=2
  fi
  wait "${RA_JOB}"
  [ -n "$T" ] && kill "$T" 2>/dev/null
  rm -rf /tmp/lumen-demo
  slog "demo end: watcher=${w}"
  case $w in 1) ARGS="--focus-demo" ;; *) ARGS="--attract" ;; esac
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

ARGS=""
while true; do
  rm -f /tmp/lumen-launch
  # ARGS is a single word or empty (--attract / --focus-demo)
  SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}" "${LUMEN_HOME}/lumen" \
    --home "${LUMEN_HOME}" --log "${LOG}/lumen.log" ${ARGS}
  rc=$?
  ARGS=""
  slog "lumen exit=${rc}"
  case "${rc}" in 10|11) ;; *) break ;; esac
  lumen_parse_launch /tmp/lumen-launch || { slog "launch request missing or invalid"; break; }
  # the exit code wins over the file
  if [ "${rc}" = "11" ]; then L_MODE=demo; else L_MODE=normal; fi
  L_OPT_FILE=""
  if [ -n "${L_OPT_CORE_NAME}" ]; then
    lumen_merge_opt "${LUMEN_CORE_OPT_DIR}/${L_OPT_CORE_NAME}/${L_OPT_CORE_NAME}.opt" /tmp/lumen-opt.kv ||
      slog "core options merge failed for ${L_OPT_CORE_NAME}"
    # game_specific_options: a <core>/<content name>.opt takes precedence over the preset
    gso=${L_CONTENT##*/}; gso="${LUMEN_CORE_OPT_DIR}/${L_OPT_CORE_NAME}/${gso%.*}.opt"
    [ -n "${LUMEN_CORE_OPT_DIR}" ] && [ -f "${gso}" ] &&
      slog "warning: ${gso} exists and overrides the graphics preset for this game"
  fi
  if [ "${L_MODE}" = demo ]; then
    lumen_run_demo
    continue
  fi
  lumen_profile_dirs
  lumen_build_ra_extra
  slog "launch: ${L_CONTENT} (system ${L_SYSTEM:-?}, profile ${L_PROFILE}, saves ${L_SAVES:-default}, graphics ${L_GRAPHICS:-default}${L_OPT_FILE:+, options ${L_OPT_FILE}})"
  run_retroarch "${L_CORE}" "${L_CONTENT}" "${LOG}/retroarch.log"
  slog "retroarch exit=$?"
done

kill "${SYNCER}" 2>/dev/null
sync
