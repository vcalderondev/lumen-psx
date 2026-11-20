#!/bin/sh
# Install a software update staged by the frontend (src/update.c) in <home>/.update/:
#
#   update.sh apply        run by boot.sh from RAM, with nothing of Lumen in use: after the
#                          session ends with 13 ("Restart and update"), or at power-on when
#                          an install was cut short. boot.sh restarts the console afterwards.
#
# <home>/.update/
#   files/<path>   every new file, already checked against the release manifest (SHA-256)
#   PLAN           what to do, one "<op> <path>" a line, paths as in the manifest
#                  (lumen/... in <home>, the rest at the stick root):
#                    d  create the folder            b  back up a boot payload file once
#                    w  move the new file in         n  move it in as <path>.new
#                    x  remove (still the file the previous release installed)
#                    r  remove the folder if it is empty
#   MANIFEST       the release's, moved to <home>/MANIFEST at the very end
#   READY          written last by the frontend: without it there is nothing to install
#
# Every step can run again: a power cut halfway through leaves READY, and the next
# power-on starts over, skipping what is done (a moved file is no longer in files/).
# Nothing outside the plan is touched: settings, profiles, saves, logs and games stay.
LUMEN_HOME="${LUMEN_HOME:-/media/lumen}"
ROOT="$(dirname "${LUMEN_HOME}")"
U="${LUMEN_HOME}/.update"
LOGF="${LUMEN_HOME}/logs/update.log"

log() { echo "$(date -u '+%F %T' 2>/dev/null) $*" >> "${LOGF}" 2>/dev/null; echo "update: $*"; }

stick() {  # manifest path -> path on the stick
  case "$1" in
    lumen) echo "${LUMEN_HOME}" ;;
    lumen/*) echo "${LUMEN_HOME}/${1#lumen/}" ;;
    *) echo "${ROOT}/$1" ;;
  esac
}

# a path from the plan: relative, no "..", nothing hidden in it
safe() {
  case "$1" in ""|/*|*..*|*'\'*) return 1 ;; esac
  return 0
}

apply() {
  [ -f "${U}/READY" ] && [ -f "${U}/PLAN" ] && [ -f "${U}/MANIFEST" ] || {
    # a download that never finished: nothing to install, nothing to keep
    [ -d "${U}" ] && { rm -rf "${U}"; log "unfinished download removed"; }
    return 0
  }
  mkdir -p "${LUMEN_HOME}/logs"
  log "installing $(cat "${U}/READY")"
  fails=0
  while read -r op p; do
    case "${op}" in d|b|w|n|x|r) ;; *) continue ;; esac
    safe "${p}" || { log "skipped unsafe path: ${p}"; continue; }
    t=$(stick "${p}")
    case "${op}" in
      d) mkdir -p "${t}" ;;
      b) bk="${LUMEN_HOME}/backup/${p}"
         if [ -f "${t}" ] && [ ! -f "${bk}" ]; then
           mkdir -p "$(dirname "${bk}")" && cp "${t}" "${bk}.tmp" && mv -f "${bk}.tmp" "${bk}" ||
             { log "backup of ${p} failed"; fails=$((fails + 1)); }
         fi ;;
      w|n) s="${U}/files/${p}"
           [ "${op}" = n ] && t="${t}.new"
           if [ -f "${s}" ]; then
             mkdir -p "$(dirname "${t}")"
             mv -f "${s}" "${t}" || { log "could not write ${p}"; fails=$((fails + 1)); }
           elif [ ! -f "${t}" ]; then
             log "missing staged file ${p}"; fails=$((fails + 1))
           fi ;;
      x) rm -f "${t}" ;;
      r) rmdir "${t}" 2>/dev/null ;;
    esac
  done < "${U}/PLAN"
  if [ "${fails}" -ne 0 ]; then
    # the old MANIFEST stays, so the frontend offers the update again and fetches only what
    # is still different. READY stays for two more power-ons, then the staging goes.
    tries=$(( $(cat "${U}/TRIES" 2>/dev/null || echo 0) + 1 ))
    if [ "${tries}" -ge 3 ]; then rm -rf "${U}"; log "install failed ${tries} times (${fails} errors): staging removed"
    else echo "${tries}" > "${U}/TRIES"; log "install incomplete (${fails} errors), tried again at the next power-on"; fi
    sync; return 1
  fi
  mv -f "${U}/MANIFEST" "${LUMEN_HOME}/MANIFEST" && sync
  rm -rf "${U}"
  sync
  log "installed"
  return 0
}

case "$1" in
  apply) apply ;;
  *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
