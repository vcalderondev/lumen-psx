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
#
# Sony's power manager unmounts the stick (umount /dev/sda1, /dev/sdb1) before it writes
# "mem" to /sys/power/state, and the stick can come back as a new device. When the session
# runs from RAM (boot.sh, /tmp/lumen-run: the POWER test path) this script does it
# properly: the stick is unmounted here, once nothing of ours uses it, and mounted again
# after the resume, before the compositor and the frontend start. Every step is logged
# to RAM and copied to <home>/logs/suspend.log when the stick is there.
HOME_DIR=/media/lumen
LOG="${HOME_DIR}/logs/session.log"
WRITERS=/tmp/lumen-usb-writers.pids # boot.sh / run.sh: log copiers, syncer

# every step goes straight to <home>/logs/suspend.log (one open/close + sync per line,
# so a hang still leaves the last step on the stick); never while the stick's writers
# are paused, which is only between "ready" and the resume
SLOG="${HOME_DIR}/logs/suspend.log"
RUN=/tmp/lumen-run                  # the session runs from RAM (boot.sh): the stick is unmounted
[ -d "${RUN}" ] && SLOG="${RUN}/suspend.log"   # RAM; logsync.sh copies it to the stick
say() { echo "$(date +%T) $*" >> "${SLOG}" 2>/dev/null; [ -d "${RUN}" ] || sync; }

# who still has something on the stick (its program, its folder or an open file)
holders() {
  for p in /proc/[0-9]*; do
    hit=""
    for l in "$p/exe" "$p/cwd" "$p"/fd/*; do
      t=$(readlink "$l" 2>/dev/null)
      case "$t" in /media/*) hit="${hit} ${t}" ;; esac
    done
    [ -n "${hit}" ] && say "suspend: on the stick: pid ${p#/proc/} ($(tr '\0' ' ' < "$p/cmdline" 2>/dev/null | cut -c1-48)):${hit}"
  done
}

# the stick again on /media: same choice as the boot payload (sda1 / sdb1, label SONY*)
stick_mount() {
  grep -q ' /media ' /proc/mounts && { echo "$(date +%T) resume: stick still mounted" >> "$R"; return 0; }
  i=0
  while [ $i -lt 40 ]; do
    dev="$(blkid 2>/dev/null | grep '^/dev/sd\(a\|b\)1:' | grep -E 'LABEL="SONY.{0,4}"' | cut -d: -f1 | head -1)"
    [ -z "${dev}" ] && [ -e /dev/disk/by-label/SONY ] && dev=/dev/disk/by-label/SONY
    if [ -n "${dev}" ] && mount -o rw "${dev}" /media 2>> "$R"; then
      echo "$(date +%T) resume: stick ${dev} mounted (try $i): $(grep ' /media ' /proc/mounts | cut -d' ' -f3,4)" >> "$R"
      return 0
    fi
    sleep 0.5; i=$((i + 1))
  done
  echo "$(date +%T) resume: NO STICK after 20 s (blkid: $(blkid 2>/dev/null | tr '\n' ' '))" >> "$R"
  return 1
}
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
  if [ -d "${RUN}" ]; then
    # nothing of ours may be left on the stick: say who is, write the logs out, pause the
    # copiers, unmount (lazily if someone still holds it: the files are synced either way)
    holders
    say "suspend: unmounting the stick ($(grep ' /media ' /proc/mounts | cut -d' ' -f1,3))"
    sh "${RUN}/logsync.sh" once
    for p in $(pids_of "${WRITERS}"); do kill -STOP "$p" 2>/dev/null; done
    sync; cd /
    if umount /media 2>> "${SLOG}"; then say "suspend: stick unmounted, then apps_okay"
    elif umount -l /media 2>> "${SLOG}"; then say "suspend: stick detached (lazy: still held), then apps_okay"
    else say "suspend: THE STICK IS STILL MOUNTED, then apps_okay"; fi
    sync
  else
    say "suspend: pausing the log copiers, then apps_okay"
    for p in $(pids_of "${WRITERS}"); do kill -STOP "$p" 2>/dev/null; done
    sync
  fi
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
  if [ -d "${RUN}" ]; then
    # what the kernel did with the USB bus and the stick, before anything touches it
    { echo "--- kernel at resume"; dmesg | tail -80; echo "--- block devices"; ls -l /dev/sd* 2>&1
      echo "--- mounts"; grep -E ' /media |fuse' /proc/mounts; } >> "$R" 2>&1
    if ! stick_mount; then
      # Lumen lives on the stick: without it nothing can start. boot.sh restarts the console
      cat "$R" >> "${SLOG}"
      : > /tmp/lumen-resume-failed
      echo 1 > /sys/class/leds/red/brightness 2>/dev/null
      rm -f /tmp/lumen-suspending; rmdir /tmp/lumen-resuming
      exit 1
    fi
  fi
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
