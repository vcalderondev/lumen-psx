#!/bin/sh
# POWER button actions of a Lumen session (run by lumen-buttons from a copy in /tmp:
# the USB stick is unmounted while powering off, so the script must not live there).
#
#   power.sh off       power the console off: quit the game (RetroArch saves on quit),
#                      close Lumen, sync and unmount the USB stick, power off
#   power.sh suspend   before Sony's suspend to RAM: sync and pause every process that
#                      reads or writes the USB stick (exFAT over FUSE: a task waiting on
#                      it cannot be frozen, so the suspend would hang)
#   power.sh resume    after the resume: let them run again
#
# Settings > POWER button chooses between off (default) and suspend (power_button in
# settings.ini); lumen-buttons calls this script accordingly.
HOME_DIR=/media/lumen
LOG="${HOME_DIR}/logs/session.log"
PIDS=/tmp/lumen-session.pids        # boot.sh / run.sh: the session shells
WRITERS=/tmp/lumen-usb-writers.pids # boot.sh / run.sh: log copiers, syncer

say() { echo "$(date +%T) power: $*" >> "${LOG}" 2>/dev/null; }
pids_of() { cat "$1" 2>/dev/null; }
wait_gone() {  # name tenths
  i=0; while [ $i -lt "$2" ] && pidof "$1" >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
}

case "$1" in
off)
  say "off: closing the session"
  # nothing may restart the frontend or start Sony's menu while we power off
  for p in $(pids_of "${PIDS}"); do kill -STOP "$p" 2>/dev/null; done
  if pidof retroarch >/dev/null 2>&1; then
    killall -s TERM retroarch 2>/dev/null; wait_gone retroarch 80   # saves SRAM + state
    killall -s KILL retroarch 2>/dev/null
  fi
  killall -s TERM lumen 2>/dev/null; wait_gone lumen 30; killall -s KILL lumen 2>/dev/null
  say "off: unmounting the USB stick, powering off"
  for p in $(pids_of "${WRITERS}"); do kill -TERM "$p" 2>/dev/null; done
  sync; sleep 0.3
  cd / && { umount /media 2>/dev/null || umount -l /media 2>/dev/null; }
  sync
  echo 0 > /sys/class/leds/green/brightness 2>/dev/null
  echo 1 > /sys/class/leds/red/brightness 2>/dev/null
  poweroff; sleep 15; poweroff -f
  ;;
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
  echo "usage: $0 off|suspend|resume" >&2; exit 2 ;;
esac
