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
# Every finished normal (non-demo) session appends one line to the play log
# $LUMEN_HOME/profiles/<profile>/playlog.tsv, read by the frontend (src/stats.h):
#   <start epoch>\t<seconds>\t<system id>\t<content path>
#
# Optional $LUMEN_HOME/lumen.conf (sourced) can set:
#   RETROARCH            path to the retroarch binary (default: retroarch in PATH)
#   LUMEN_MAIN_SAVES     save folder of the "main" profile (empty: RetroArch decides)
#   LUMEN_MAIN_STATES    save-state folder of the "main" profile
#   LUMEN_CORE_OPT_DIR   folder holding RetroArch's <Core Name>/<Core Name>.opt
#                        (graphics presets are merged there; empty: not merged)
#   LUMEN_RESUME_SLOT    RetroArch save-state slot of "Save and exit" (default 9): every
#                        normal session starts on it, a resumed one loads it
#   run_retroarch()      function(core, content, logfile) to launch a game; it must
#                        apply the extra config in $LUMEN_RA_EXTRA (RetroArch cfg lines),
#                        pass --entryslot $LUMEN_RA_ENTRYSLOT when it is set (resume) and
#                        --set-shader $LUMEN_RA_SHADER when it is set (CRT filter)
#
# Save and exit (OPEN in a game, lumen-buttons): the state goes to the resume slot and
# /tmp/lumen-save-exit is left; after RetroArch exits the content is recorded in
# $LUMEN_HOME/profiles/<profile>/resume.tsv ("<content>\t<epoch>"), so the frontend asks
# "continue?" next time (launch request resume=1 -> --entryslot).
# Homebrew apps (launch request kind=app): content is a native program, started from its
# folder with the frontend's video driver; its pid is in /tmp/lumen-app.pid (RESET / OPEN
# quit it, lumen-buttons).
LUMEN_HOME="${LUMEN_HOME:-$(cd "$(dirname "$0")" && pwd)}"
LOG="${LUMEN_HOME}/logs"
RETROARCH="${RETROARCH:-retroarch}"
LUMEN_MAIN_SAVES=""
LUMEN_MAIN_STATES=""
LUMEN_CORE_OPT_DIR=""
LUMEN_RESUME_SLOT=9
LUMEN_RA_EXTRA=/tmp/lumen-ra-extra.cfg
mkdir -p "${LOG}"
chmod +x "${LUMEN_HOME}/lumen" 2>/dev/null
[ -z "${XDG_RUNTIME_DIR}" ] && export XDG_RUNTIME_DIR=/run/user/0

slog() { echo "$*" >> "${LOG}/session.log"; }

# lumen_parse_launch FILE: sets L_CORE L_CONTENT L_SYSTEM L_TITLE L_PROFILE L_MODE
# L_DEMO_SECONDS L_GRAPHICS L_OPT_CORE_NAME L_CRT; writes the opt.* pairs to /tmp/lumen-opt.kv
# and the ra.* pairs to /tmp/lumen-ra.kv ("key=value" lines). Fails without core/content.
lumen_parse_launch() {
  L_CORE=""; L_CONTENT=""; L_SYSTEM=""; L_TITLE=""; L_PROFILE=main; L_MODE=normal
  L_DEMO_SECONDS=60; L_GRAPHICS=""; L_OPT_CORE_NAME=""; L_RESUME=0; L_KIND=game; L_CRT=""
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
          resume) L_RESUME=$val ;;
          kind) L_KIND=$val ;;
          crt) L_CRT=$val ;;
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
  case "$L_RESUME" in 1) ;; *) L_RESUME=0 ;; esac
  case "$L_KIND" in app) ;; *) L_KIND=game ;; esac
  case "$L_CRT" in soft|strong|soft-quality|strong-quality) ;; *) L_CRT="" ;; esac
  [ "$L_KIND" = app ] && [ -n "$L_CONTENT" ] && return 0
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
  # merged in RAM: appending line by line on the stick (exFAT through FUSE) was one
  # open + write + close per option, and the result is usually what is already there
  mo=/tmp/lumen-opt.merge
  : > "$mo" || return 1
  seen="|"
  if [ -f "$1" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      case "$line" in
        *" ="*)
          key=${line%% =*}
          if lumen_kv_get "$key" "$2"; then
            echo "${key} = \"${KV_VAL}\"" >> "$mo"; seen="${seen}${key}|"; continue
          fi ;;
      esac
      echo "$line" >> "$mo"
    done < "$1"
  fi
  while IFS= read -r kvl || [ -n "$kvl" ]; do
    key=${kvl%%=*}
    case "$seen" in *"|${key}|"*) continue ;; esac
    lumen_kv_get "$key" "$2"
    echo "${key} = \"${KV_VAL}\"" >> "$mo"; seen="${seen}${key}|"
  done < "$2"
  L_OPT_FILE=$1
  # same preset as the last launch: nothing to write, nothing to sync
  if [ -f "$1" ] && cmp -s "$mo" "$1" 2>/dev/null; then rm -f "$mo"; return 0; fi
  cp "$mo" "$1.tmp" && mv -f "$1.tmp" "$1" || { L_OPT_FILE=""; return 1; }
  rm -f "$mo"
  sync
}

# lumen_build_ra_extra: RetroArch cfg lines that must win over its own config
# (profile save folders, the graphics preset's ra.* overrides, demo settings); it is
# handed over with --appendconfig. RetroArch keeps the FIRST copy of a key within one
# file, so these lines must never just be concatenated after its retroarch.cfg.
# The merged .opt needs no line here: RetroArch reads its per-core file by itself.
# CRT filter (request crt=soft|strong, or soft-quality|strong-quality when settings.ini has
# crt_shader=quality: the heavier maths): shaders on, before the ra.* lines so it wins, and
# the preset in LUMEN_RA_SHADER for run_retroarch's --set-shader (RetroArch 1.22 has no
# cfg key for the shader path: --set-shader acts as a global auto-shader preset).
lumen_build_ra_extra() {
  : > "${LUMEN_RA_EXTRA}"
  LUMEN_RA_SHADER=""
  if [ -n "$L_CRT" ]; then
    if [ -f "${LUMEN_HOME}/shaders/crt-lumen-${L_CRT}.glslp" ]; then
      LUMEN_RA_SHADER="${LUMEN_HOME}/shaders/crt-lumen-${L_CRT}.glslp"
      echo 'video_shader_enable = "true"' >> "${LUMEN_RA_EXTRA}"
    else
      slog "crt: ${LUMEN_HOME}/shaders/crt-lumen-${L_CRT}.glslp missing, game without the filter"
    fi
  fi
  export LUMEN_RA_SHADER
  [ -n "$L_SAVES" ] && echo "savefile_directory = \"${L_SAVES}\"" >> "${LUMEN_RA_EXTRA}"
  [ -n "$L_STATES" ] && echo "savestate_directory = \"${L_STATES}\"" >> "${LUMEN_RA_EXTRA}"
  if [ "${LUMEN_DISPLAY}" = 1080p ]; then   # boot.sh started Weston at 1920x1080
    echo 'video_fullscreen_x = "1920"' >> "${LUMEN_RA_EXTRA}"
    echo 'video_fullscreen_y = "1080"' >> "${LUMEN_RA_EXTRA}"
  fi
  if [ -f /tmp/lumen-ra.kv ]; then
    while IFS= read -r kvl || [ -n "$kvl" ]; do
      [ -n "$kvl" ] && echo "${kvl%%=*} = \"${kvl#*=}\"" >> "${LUMEN_RA_EXTRA}"
    done < /tmp/lumen-ra.kv
  fi
  if [ "$L_MODE" != demo ]; then   # Save and exit writes this slot (lumen-buttons SAVE_STATE)
    echo "state_slot = \"${LUMEN_RESUME_SLOT}\"" >> "${LUMEN_RA_EXTRA}"
    echo 'savestate_auto_index = "false"' >> "${LUMEN_RA_EXTRA}"
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

# lumen_discs: RA_CONTENT = what RetroArch loads. A game folder with several discs
# (disc 1 is launched: <id>.cue, the other discs <id2>.cue ... in Game.ini Discs=, or every
# .cue in the folder) becomes an .m3u playlist in /tmp named like disc 1 (same save and
# state names), so RetroArch can swap discs (OPEN). The disc count goes to /tmp/lumen-discs.
lumen_discs() {
  RA_CONTENT="${L_CONTENT}"; n=1
  case "${L_CONTENT}" in
    *.cue|*.CUE)
      dir=${L_CONTENT%/*}; base=${L_CONTENT##*/}; base=${base%.*}
      list=/tmp/lumen-m3u/${base}.m3u; mkdir -p /tmp/lumen-m3u; : > "${list}"
      discs=$(sed -n 's/^Discs=//p' "${dir}/Game.ini" 2>/dev/null | tr -d '\r' | tr ',' ' ')
      for d in ${discs}; do [ -f "${dir}/${d}.cue" ] && echo "${dir}/${d}.cue" >> "${list}"; done
      # no Discs= (or its ids are not the file names): every .cue in the folder, sorted
      [ "$(grep -c . "${list}")" -gt 1 ] || ls "${dir}"/*.cue "${dir}"/*.CUE 2>/dev/null | sort > "${list}"
      n=$(grep -c . "${list}")
      # the launched disc first (it is disc 1 in every layout Lumen builds)
      if [ "${n}" -gt 1 ] && grep -qxF "${L_CONTENT}" "${list}"; then RA_CONTENT=${list}; else n=1; fi ;;
    *.m3u|*.M3U) n=$(grep -c '^[^#]' "${L_CONTENT}" 2>/dev/null || echo 1) ;;
  esac
  echo "${n}" > /tmp/lumen-discs
  echo 1 > /tmp/lumen-disc-current
  [ "${n}" -gt 1 ] && slog "discs: ${n} (${RA_CONTENT})"
}

# lumen_playlog T0: append the session that started at T0 to the profile's play log
lumen_playlog() {
  case "$1" in ''|*[!0-9]*) return 0 ;; esac
  pl_now=$(date +%s)
  case "$pl_now" in ''|*[!0-9]*) return 0 ;; esac
  pl_secs=$(( pl_now - $1 ))
  # under 10 s is a failed start, not a session; over 30 days the clock jumped (NTP)
  [ "$pl_secs" -ge 10 ] && [ "$pl_secs" -le 2592000 ] || return 0
  pl_dir="${LUMEN_HOME}/profiles/${L_PROFILE}"
  mkdir -p "$pl_dir" || return 0
  pl_sys=$(printf '%s' "$L_SYSTEM" | tr -d '\t\r\n')
  pl_con=$(printf '%s' "$L_CONTENT" | tr -d '\t\r\n')
  printf '%s\t%s\t%s\t%s\n' "$1" "$pl_secs" "$pl_sys" "$pl_con" >> "${pl_dir}/playlog.tsv" ||
    slog "play log write failed"
}

run_retroarch() {  # core content logfile
  "${RETROARCH}" --appendconfig "${LUMEN_RA_EXTRA}" ${LUMEN_RA_ENTRYSLOT:+--entryslot "${LUMEN_RA_ENTRYSLOT}"} \
    ${LUMEN_RA_SHADER:+--set-shader "${LUMEN_RA_SHADER}"} -L "$1" "$2" > "$3" 2>&1
}

# lumen_resume_mark T0REF: after Save and exit, record the content in the profile's
# resume.tsv (one line per content, the newest wins). With a known state folder the
# slot's file must be newer than the launch, else the save did not happen.
lumen_resume_mark() {
  [ -f /tmp/lumen-save-exit ] || return 0
  rm -f /tmp/lumen-save-exit
  rm_base=${L_CONTENT##*/}; rm_base=${rm_base%.*}
  if [ -n "$L_STATES" ] && [ -d "$L_STATES" ]; then
    found=$(find "$L_STATES" -name "${rm_base}.state${LUMEN_RESUME_SLOT}" -newer "$1" 2>/dev/null | head -1)
    if [ -z "$found" ]; then slog "save and exit: no new ${rm_base}.state${LUMEN_RESUME_SLOT} in ${L_STATES}, not marked"; return 0; fi
  fi
  rm_dir="${LUMEN_HOME}/profiles/${L_PROFILE}"
  mkdir -p "$rm_dir" || return 0
  rm_con=$(printf '%s' "$L_CONTENT" | tr -d '\t\r\n')
  rm_tab=$(printf '\t')
  { [ -f "$rm_dir/resume.tsv" ] && grep -vF "${rm_con}${rm_tab}" "$rm_dir/resume.tsv"
    printf '%s\t%s\n' "$rm_con" "$(date +%s)"; } > "$rm_dir/resume.tsv.tmp" &&
    mv -f "$rm_dir/resume.tsv.tmp" "$rm_dir/resume.tsv"
  sync
  slog "save and exit: ${rm_con} (slot ${LUMEN_RESUME_SLOT})"
}

# lumen_run_app: a native homebrew program (kind=app), from its own folder. FAT32 sticks
# may not keep the exec bit: the program then runs from a copy in /tmp.
lumen_run_app() {
  app_dir=${L_CONTENT%/*}; app_bin=${L_CONTENT}
  if [ ! -x "$app_bin" ]; then
    chmod +x "$app_bin" 2>/dev/null
    [ -x "$app_bin" ] || { cp -f "$app_bin" /tmp/lumen-app-bin && chmod +x /tmp/lumen-app-bin && app_bin=/tmp/lumen-app-bin; }
  fi
  slog "app: ${L_CONTENT} (${L_TITLE:-?})"
  ( cd "$app_dir" && LUMEN_APP_DIR="$app_dir" SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}" exec "$app_bin" ) \
    > "${LOG}/app.log" 2>&1 &
  app_pid=$!
  echo "$app_pid" > /tmp/lumen-app.pid
  wait "$app_pid"
  slog "app exit=$?"
  rm -f /tmp/lumen-app.pid /tmp/lumen-app-bin
}
[ -f "${LUMEN_HOME}/lumen.conf" ] && . "${LUMEN_HOME}/lumen.conf"

( while true; do sleep 3; sync; done ) &
SYNCER=$!
echo "${SYNCER}" >> /tmp/lumen-usb-writers.pids
echo "lumen session $(date)" > "${LOG}/session.log"
# Kernel read-ahead of the stick: 2 MB instead of the default 128 KB. A game that streams
# from a disc image asks for it in small reads a second or two apart, and on a slow stick
# each of them takes ~200 ms (plus ~500 ms behind any small write): the emulation stopped
# ~300 ms every couple of seconds. With 2 MB the kernel reads ahead in long bursts, before
# the game needs the data. LUMEN_MOUNTS / LUMEN_SYS: for tests.
usb_readahead() {
  dev=$(awk -v h="${LUMEN_HOME}/" '$2 != "/" && index(h, $2 "/") == 1 && length($2) > n { n = length($2); d = $1 }
        END { print d }' "${LUMEN_MOUNTS:-/proc/mounts}" 2>/dev/null)
  blk=$(echo "${dev#/dev/}" | sed 's/[0-9]*$//')
  ra="${LUMEN_SYS:-/sys}/block/${blk}/queue/read_ahead_kb"
  [ -n "${blk}" ] && [ -w "${ra}" ] || return 0
  [ "$(cat "${ra}")" -ge 2048 ] 2>/dev/null && return 0
  echo 2048 > "${ra}" && slog "usb: read-ahead of ${blk} set to 2048 kB"
}
usb_readahead
# profiles deleted in the frontend whose background removal did not finish
rm -rf "${LUMEN_HOME}"/profiles/.trash-* 2>/dev/null

ARGS=""
# POWER (power.sh suspend) closes the game and the frontend and stops the compositor;
# /tmp/lumen-suspending stays until power.sh resume has started it again
suspend_wait() {
  [ -f /tmp/lumen-suspending ] || return 1
  slog "suspended: waiting for the resume"
  while [ -f /tmp/lumen-suspending ]; do sleep 0.5; done
  # the stick did not come back: boot.sh restarts the console
  [ -f /tmp/lumen-resume-failed ] && exit 3
  slog "resumed"
  return 0
}
while true; do
  suspend_wait
  rm -f /tmp/lumen-launch
  # ARGS is a single word or empty (--attract / --focus-demo)
  # keep the previous frontend log (benchmarks run before a game would be lost)
  [ -s "${LOG}/lumen.log" ] && mv -f "${LOG}/lumen.log" "${LOG}/lumen-prev.log"
  SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}" "${LUMEN_HOME}/lumen" \
    --home "${LUMEN_HOME}" --log "${LOG}/lumen.log" ${ARGS}
  rc=$?
  ARGS=""
  slog "lumen exit=${rc}"
  # RESET in the dashboard (lumen-buttons): start the frontend again
  if [ -f /tmp/lumen-restart ]; then rm -f /tmp/lumen-restart; slog "dashboard restarted (RESET)"; continue; fi
  case "${rc}" in 10|11) ;; *) suspend_wait && continue; break ;; esac
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
  if [ "${L_KIND}" = app ]; then
    t0=$(date +%s)
    lumen_run_app
    lumen_playlog "$t0"
    continue
  fi
  lumen_build_ra_extra
  LUMEN_RA_ENTRYSLOT=""
  [ "${L_RESUME}" = 1 ] && LUMEN_RA_ENTRYSLOT=${LUMEN_RESUME_SLOT}
  export LUMEN_RA_ENTRYSLOT
  rm -f /tmp/lumen-save-exit; : > /tmp/lumen-t0
  slog "launch: ${L_CONTENT} (system ${L_SYSTEM:-?}, profile ${L_PROFILE}, saves ${L_SAVES:-default}, graphics ${L_GRAPHICS:-default}${L_CRT:+, crt ${L_CRT}}${L_OPT_FILE:+, options ${L_OPT_FILE}}${LUMEN_RA_ENTRYSLOT:+, resume slot ${LUMEN_RA_ENTRYSLOT}})"
  # a truncated copy is the usual reason a game never starts: keep its size in the log
  slog "content: $(wc -c < "${L_CONTENT}" 2>/dev/null || echo '?') bytes"
  t0=$(date +%s)
  lumen_discs
  run_retroarch "${L_CORE}" "${RA_CONTENT}" "${LOG}/retroarch.log"
  slog "retroarch exit=$?"
  lumen_resume_mark /tmp/lumen-t0
  lumen_playlog "$t0"
done

kill "${SYNCER}" 2>/dev/null
sync
# 12: the frontend asked for a restart (Display settings); boot.sh restarts the console
[ "${rc}" = 12 ] && exit 12
exit 0
