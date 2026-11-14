#!/bin/sh
# POWER button around Sony's suspend to RAM (run by lumen-buttons from a copy in /tmp,
# so it never waits on the USB stick itself).
#
#   power.sh suspend   before Sony's suspend to RAM: close the game (saved) and the
#                      frontend, stop the compositor, pause the USB log copiers (exFAT
#                      over FUSE: a task waiting on it cannot be frozen)
#   power.sh resume    after the resume: start the compositor again (run.sh then
#                      starts the frontend)
#
# The PSC has no software power-off (a halted console stays orange until unplugged), so
# POWER is always Sony's suspend.
HOME_DIR=/media/lumen
LOG="${HOME_DIR}/logs/session.log"
WRITERS=/tmp/lumen-usb-writers.pids # boot.sh / run.sh: log copiers, syncer

say() { echo "$(date +%T) power: $*" >> "${LOG}" 2>/dev/null; }
pids_of() { cat "$1" 2>/dev/null; }

wait_gone() {  # name tenths
  i=0; while [ $i -lt "$2" ] && pidof "$1" >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
}

case "$1" in
suspend)
  # the PowerVR / display driver cannot suspend under a running GL compositor (Sony's
  # power manager stops its own Weston for that): close the game (RetroArch saves SRAM
  # and the automatic state on quit) and the frontend, stop the compositor; run.sh waits
  # for /tmp/lumen-suspending to go and starts the frontend again
  : > /tmp/lumen-suspending
  say "suspend: closing the session"
  if pidof retroarch >/dev/null 2>&1; then
    killall -s TERM retroarch 2>/dev/null; wait_gone retroarch 80; killall -s KILL retroarch 2>/dev/null
  fi
  killall -s TERM lumen 2>/dev/null; wait_gone lumen 30; killall -s KILL lumen 2>/dev/null
  killall -s TERM weston 2>/dev/null; wait_gone weston 30; killall -s KILL weston 2>/dev/null
  sync
  for p in $(pids_of "${WRITERS}"); do kill -STOP "$p" 2>/dev/null; done
  sync
  ;;
resume)
  # run by Sony's resume_run (bind-mounted by boot.sh) and by lumen-buttons when
  # resume_count moves: the first one does it
  [ -f /tmp/lumen-suspending ] || exit 0
  mkdir /tmp/lumen-resuming 2>/dev/null || exit 0
  echo 1 > /sys/class/leds/green/brightness 2>/dev/null
  echo 0 > /sys/class/leds/red/brightness 2>/dev/null
  for p in $(pids_of "${WRITERS}"); do kill -CONT "$p" 2>/dev/null; done
  say "resume: starting the compositor"
  sh /tmp/lumen-weston-start.sh
  rm -f /tmp/lumen-suspending; rmdir /tmp/lumen-resuming
  ;;
*)
  echo "usage: $0 suspend|resume" >&2; exit 2 ;;
esac
