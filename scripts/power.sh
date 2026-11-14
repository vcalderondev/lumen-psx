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

# every step goes straight to <home>/logs/suspend.log (one open/close + sync per line,
# so a hang still leaves the last step on the stick); never while the stick's writers
# are paused, which is only between "ready" and the resume
SLOG="${HOME_DIR}/logs/suspend.log"
say() { echo "$(date +%T) $*" >> "${SLOG}" 2>/dev/null; sync; }
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
  say "=== suspend (POWER)"
  say "suspend: closing the session"
  : > /tmp/lumen-suspending
  if pidof retroarch >/dev/null 2>&1; then
    killall -s TERM retroarch 2>/dev/null; wait_gone retroarch 80; killall -s KILL retroarch 2>/dev/null
  fi
  killall -s TERM lumen 2>/dev/null; wait_gone lumen 30; killall -s KILL lumen 2>/dev/null
  say "suspend: game and frontend closed, stopping the compositor"
  killall -s TERM weston 2>/dev/null; wait_gone weston 30; killall -s KILL weston 2>/dev/null
  say "suspend: compositor stopped ($(pidof weston >/dev/null && echo STILL RUNNING || echo gone))"
  # test switch: <home>/debug/rtc-wake makes the RTC wake the console after 20 s, to tell
  # "POWER does not wake it" from "the resume itself hangs"
  if [ -f "${HOME_DIR}/debug/rtc-wake" ]; then
    echo 0 > /sys/class/rtc/rtc0/wakealarm 2>/dev/null; echo +20 > /sys/class/rtc/rtc0/wakealarm 2>/dev/null
    say "suspend: RTC wake in 20 s ($(cat /sys/class/rtc/rtc0/wakealarm 2>/dev/null))"
  fi
  say "suspend: pausing the log copiers, then apps_okay"
  for p in $(pids_of "${WRITERS}"); do kill -STOP "$p" 2>/dev/null; done
  sync
  ;;
resume)
  # run by Sony's resume_run (bind-mounted by boot.sh) and by lumen-buttons when
  # resume_count moves: the first one does it
  [ -f /tmp/lumen-suspending ] || exit 0
  mkdir /tmp/lumen-resuming 2>/dev/null || exit 0
  # RAM only until the compositor is up (the stick may come back late or not at all):
  # green LED = woke up and resumed; the steps are written to the stick afterwards
  echo 1 > /sys/class/leds/green/brightness 2>/dev/null
  echo 0 > /sys/class/leds/red/brightness 2>/dev/null
  R=/tmp/lumen-resume.log
  echo "$(date +%T) resume (${2:-?}): woke up" > "$R"
  sh /tmp/lumen-weston-start.sh
  echo "$(date +%T) resume: compositor $([ -S /run/user/0/wayland-0 ] && echo up || echo NOT UP)" >> "$R"
  for p in $(pids_of "${WRITERS}"); do kill -CONT "$p" 2>/dev/null; done
  rm -f /tmp/lumen-suspending; rmdir /tmp/lumen-resuming
  ( { cat "$R"; echo "--- mounts"; grep -E ' /media |fuse' /proc/mounts; echo "--- power_manage"; tail -25 /tmp/power.log
      echo "--- kernel"; dmesg | tail -150; } >> "${SLOG}" 2>/dev/null; sync ) &
  ;;
*)
  echo "usage: $0 suspend|resume" >&2; exit 2 ;;
esac
