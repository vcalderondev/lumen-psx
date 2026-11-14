#!/bin/sh
# POWER button around Sony's suspend to RAM (run by lumen-buttons from a copy in /tmp,
# so it never waits on the USB stick itself).
#
#   power.sh suspend   before Sony's suspend to RAM: sync and pause every process that
#                      reads or writes the USB stick (exFAT over FUSE: a task waiting on
#                      it cannot be frozen, so the suspend would hang)
#   power.sh resume    after the resume: let them run again
#
# The PSC has no software power-off (a halted console stays orange until unplugged), so
# POWER is always Sony's suspend.
HOME_DIR=/media/lumen
LOG="${HOME_DIR}/logs/session.log"
WRITERS=/tmp/lumen-usb-writers.pids # boot.sh / run.sh: log copiers, syncer

say() { echo "$(date +%T) power: $*" >> "${LOG}" 2>/dev/null; }
pids_of() { cat "$1" 2>/dev/null; }

case "$1" in
suspend)
  sync
  for n in retroarch lumen; do for p in $(pidof "$n"); do kill -STOP "$p" 2>/dev/null; done; done
  for p in $(pids_of "${WRITERS}"); do kill -STOP "$p" 2>/dev/null; done
  sync
  ;;
resume)
  for p in $(pids_of "${WRITERS}"); do kill -CONT "$p" 2>/dev/null; done
  for n in lumen retroarch; do for p in $(pidof "$n"); do kill -CONT "$p" 2>/dev/null; done; done
  say "resumed"
  ;;
*)
  echo "usage: $0 suspend|resume" >&2; exit 2 ;;
esac
