#!/bin/sh
# Lumen boot entry for the PlayStation Classic.
#
# Called once at power-on, after the USB stick (label SONY) is mounted on /media
# and the console's own UI has been stopped. It must never simply return: the
# caller reboots the console when this script ends.
#
#  1. stop the Sony UI processes and make sure Weston (Wayland) is running
#  2. run a Lumen session (frontend <-> RetroArch loop, see run.sh)
#  3. when the session ends (Select+Start) or Lumen fails, hand the console to
#     the stock Sony menu so it always stays usable; the power button works there.
LUMEN_HOME="/media/lumen"
LOG="${LUMEN_HOME}/logs"
mkdir -p "${LOG}"
exec >> "${LOG}/boot.log" 2>&1
echo "=== lumen boot $(date)"
# CPU / GPU frequency policy, for the record (read only): governor, current and
# available frequencies of cpu0, and of every devfreq device (the GPU's, if any)
for f in scaling_governor scaling_cur_freq scaling_min_freq scaling_max_freq scaling_available_governors scaling_available_frequencies; do
  [ -r "/sys/devices/system/cpu/cpu0/cpufreq/$f" ] && echo "cpu0 $f: $(cat "/sys/devices/system/cpu/cpu0/cpufreq/$f" 2>/dev/null)"
done
for d in /sys/class/devfreq/*; do
  [ -d "$d" ] || continue
  for f in governor cur_freq min_freq max_freq available_governors available_frequencies; do
    [ -r "$d/$f" ] && echo "devfreq $(basename "$d") $f: $(cat "$d/$f" 2>/dev/null)"
  done
done
[ -d /sys/class/devfreq ] || echo "devfreq: none (no /sys/class/devfreq)"

export XDG_RUNTIME_DIR=/run/user/0
export HOME=/home/root
cd /home/root

# 1. take over the display
killall -s TERM sdl_display 2>/dev/null
killall -s KILL showLogo sonyapp ui_menu auto_dimmer pcsx 2>/dev/null

# Output resolution: Sony's weston.ini forces HDMI 1280x720. With display=1080p in
# settings.ini, Weston is restarted with a copy of that config set to 1920x1080
# (/tmp, nothing on the eMMC changes). The console's HDMI driver reads no EDID (the
# connector has no mode list), so a plain "1920x1080" is ignored and Weston keeps its
# built-in 720p: the mode is given as an explicit CEA-861 modeline (1080p60, 148.5 MHz).
# If Weston does not come up, or does not report 1920x1080 as the current mode,
# back to the stock 720p.
MODE_1080P="148.50 1920 2008 2052 2200 1080 1084 1089 1125 +hsync +vsync"
MODE_720P="74.25 1280 1390 1430 1650 720 725 730 750 +hsync +vsync"   # CEA-861 720p60
LUMEN_WESTON=""
# Compositor: the PSC compositor built from source by psc-platform, newest installed
# first: psc-weston-16 (Weston 16.0.2), then psc-weston-6 (Weston 6.0.1; psc-weston.tar.gz
# in earlier installs). Not a user choice (RetroArch 1.22 needs it); Sony's Weston 1.11
# is only the fallback, or forced by hand with compositor=stock in settings.ini. The
# package is unpacked to RAM and overlaid on /usr (overlayfs: nothing on the eMMC
# changes, gone at power-off).
PW_VER=""; PW_ON=""; PW_SAMPLER=""
# Guard: <home>/compositor/.trying holds the version being tried and is removed once the
# frontend has presented frames. Still there at boot = that attempt never showed a
# picture (crash, black screen, power-off): it becomes .failed-<version> and that
# version is skipped from then on (the installer clears it, or delete it by hand).
PW_MARK="${LUMEN_HOME}/compositor/.trying"
pw_tar() {  # version -> package path
  for f in "psc-weston-$1.tar.gz" $( [ "$1" = 6 ] && echo psc-weston.tar.gz ); do
    [ -f "${LUMEN_HOME}/compositor/$f" ] && { echo "${LUMEN_HOME}/compositor/$f"; return 0; }
  done
  return 1
}
# stop Weston politely first: Weston 16's libseat gives the VT back (text mode, keyboard)
# only when it exits normally; a SIGKILL leaves it in graphics mode
pw_stop_weston() {
  killall -s TERM weston 2>/dev/null
  i=0; while [ $i -lt 30 ] && pidof weston >/dev/null 2>&1; do sleep 0.1; i=$((i + 1)); done
  killall -s KILL weston 2>/dev/null; sleep 0.5
}
psc_weston_off() {
  [ -n "${PW_ON}" ] || return 0
  [ -n "${PW_SAMPLER}" ] && kill "${PW_SAMPLER}" 2>/dev/null
  pw_stop_weston
  if umount -l /usr; then rm -rf /tmp/pw_upper /tmp/pw_work; echo "psc compositor overlay removed"
  else echo "psc compositor: umount /usr failed"; fi
  PW_ON=""; LUMEN_WESTON=""
}
weston_stock() {
  psc_weston_off
  [ -n "${LUMEN_WESTON}" ] && { killall -s KILL weston 2>/dev/null; sleep 1; LUMEN_WESTON=""; }
  if ! systemctl status weston.service | grep -qE 'active\s+\(running\)'; then
    echo "starting weston (stock)"
    systemctl restart weston.service
  fi
}
weston_up() {  # seconds: wait for the Wayland socket
  i=0
  while [ $i -lt $(( $1 * 10 )) ]; do [ -S "${XDG_RUNTIME_DIR}/wayland-0" ] && return 0; sleep 0.1; i=$((i + 1)); done
  return 1
}
weston_custom() {  # log file, extra weston args...: start Weston with /tmp/lumen-weston.ini
  wlog=$1; shift
  systemctl stop weston.service
  killall -s KILL weston 2>/dev/null
  rm -f "${XDG_RUNTIME_DIR}/wayland-0" "${XDG_RUNTIME_DIR}/wayland-0.lock"
  /usr/bin/weston-launch -u root -- --idle-time=4294967 --tty=1 --config=/tmp/lumen-weston.ini "$@" \
    > "${wlog}" 2>&1 &
  LUMEN_WESTON=$!
}
setting() { sed -n "s/^$1=//p" "${LUMEN_HOME}/settings.ini" 2>/dev/null | tr -d '\r' | tail -1; }
DISPLAY_MODE=$(setting display)
COMPOSITOR=$(setting compositor)
[ -f /etc/xdg/weston/weston.ini ] || { DISPLAY_MODE=720p; COMPOSITOR=""; }
if [ "${DISPLAY_MODE}" = 1080p ]; then
  sed "/^name=HDMI-A-1/{n;s/^mode=.*/mode=${MODE_1080P}/;}" /etc/xdg/weston/weston.ini > /tmp/lumen-weston.ini
else
  cp /etc/xdg/weston/weston.ini /tmp/lumen-weston.ini 2>/dev/null
fi
STARTED=""
# Weston 6 and 16 settings for the PSC: the shell key that hides the panel was renamed;
# the HDMI connector reports "disconnected" with no EDID at boot and no mode list, so the
# output is forced on and every mode is a full modeline (a plain "1280x720" finds nothing
# to match and Weston exits). pageflip-timeout: a lost page flip makes Weston exit
# instead of freezing. (Sony's 1.11 ignores these keys.)
[ "${DISPLAY_MODE}" = 1080p ] ||
  sed -i "/^name=HDMI-A-1/{n;s/^mode=.*/mode=${MODE_720P}/;}" /tmp/lumen-weston.ini
sed -i -e '/^panel-location=none/a panel-position=none' \
       -e '/^name=HDMI-A-1/a force-on=true' \
       -e '/^\[core\]/a pageflip-timeout=1500' /tmp/lumen-weston.ini

# start one PSC compositor version from its package; 0 = up with a mode set
try_psc_weston() {  # version package
  v=$1
  echo "compositor psc weston ${v} (${DISPLAY_MODE:-720p}): overlay from RAM"
  printf '%s\n' "${v}" > "${PW_MARK}"; sync
  echo "  $(date +%T) stopping Sony's weston"
  systemctl stop weston.service; pw_stop_weston
  echo "  $(date +%T) unpacking"
  mkdir -p /tmp/pw_upper/usr /tmp/pw_work/usr
  if ! tar xzpf "$2" -C /tmp/pw_upper/usr ||
     ! { echo "  $(date +%T) mounting overlay"; mount -t overlay overlay -o lowerdir=/usr,upperdir=/tmp/pw_upper/usr,workdir=/tmp/pw_work/usr /usr; }; then
    echo "psc compositor ${v}: overlay failed"; rm -rf /tmp/pw_upper /tmp/pw_work; return 1
  fi
  echo "  $(date +%T) starting weston ${v}"
  PW_ON=1
  # stock Weston's PAM session (weston-launch) owned /run/user/0; launched directly,
  # nothing creates it, and Weston refuses to start without it
  mkdir -p "${XDG_RUNTIME_DIR}" && chmod 0700 "${XDG_RUNTIME_DIR}"
  if [ "${v}" = 16 ]; then
    # Weston 16: libseat's builtin backend takes the active VT (no --tty). Atomic KMS
    # first (psc-platform patch 0001), legacy KMS (no direct scanout) if it fails.
    chvt 1 2>/dev/null
    for kms in atomic legacy; do
      rm -f "${XDG_RUNTIME_DIR}/wayland-0" "${XDG_RUNTIME_DIR}/wayland-0.lock"
      if [ "${kms}" = legacy ]; then export WESTON_DISABLE_ATOMIC=1; else unset WESTON_DISABLE_ATOMIC; fi
      # --socket: Weston 16 picks wayland-1 by default; clients (SDL, RetroArch) look for wayland-0
      LIBSEAT_BACKEND=builtin /usr/bin/weston --backend=drm --shell=desktop --idle-time=0 \
        --socket=wayland-0 --config=/tmp/lumen-weston.ini --debug > "${LOG}/weston-psc.log" 2>&1 &
      LUMEN_WESTON=$!
      if psc_weston_ready; then echo "weston 16: ${kms} KMS"; break; fi
      echo "weston 16 (${kms} KMS) did not come up: $(tail -4 "${LOG}/weston-psc.log" 2>/dev/null)"
      cp -f "${LOG}/weston-psc.log" "${LOG}/weston-psc-${kms}.log" 2>/dev/null
      pw_stop_weston; LUMEN_WESTON=""
    done
    [ -n "${LUMEN_WESTON}" ] || { psc_weston_off; return 1; }
  else
    # Weston 6: as root it opens the DRM device itself (no weston-launch in this build).
    # scanout (default on, scanout=0 to disable): a fullscreen client buffer goes to the
    # display without a GL composition pass (psc-platform patch 0002). 40 -> 58 fps.
    rm -f "${XDG_RUNTIME_DIR}/wayland-0" "${XDG_RUNTIME_DIR}/wayland-0.lock"
    if [ "$(setting scanout)" != 0 ]; then export WESTON_LEGACY_SCANOUT=1; else unset WESTON_LEGACY_SCANOUT; fi
    /usr/bin/weston --tty=1 --idle-time=0 --config=/tmp/lumen-weston.ini --debug \
      > "${LOG}/weston-psc.log" 2>&1 &
    LUMEN_WESTON=$!
    psc_weston_ready || { echo "psc compositor 6 failed to start: $(tail -5 "${LOG}/weston-psc.log" 2>/dev/null)"; psc_weston_off; return 1; }
  fi
  PW_VER=${v}
  echo "psc compositor up (${LUMEN_WESTON}): $(/usr/bin/weston --version 2>&1 | head -1)"
  grep -aiE 'video modes|, current|scanout|atomic|renderer:|enabled with head' "${LOG}/weston-psc.log" | head -8
  grep -aqE '^ +1920x1080@[0-9.]+.*, current' "${LOG}/weston-psc.log" || DISPLAY_MODE=720p
  # evidence: which plane each view lands on, sampled twice while the frontend runs
  ( sleep 14; [ -x /usr/bin/weston-debug ] || exit 0
    /usr/bin/weston-debug drm-backend > "${LOG}/weston-planes.log" 2>&1 & d=$!; sleep 2; kill $d 2>/dev/null
    sleep 20; [ -x /usr/bin/weston-debug ] || exit 0
    /usr/bin/weston-debug drm-backend >> "${LOG}/weston-planes.log" 2>&1 & d=$!; sleep 1; kill $d 2>/dev/null ) &
  PW_SAMPLER=$!
  return 0
}
psc_weston_ready() {  # socket up, process alive, an output mode set
  weston_up 8 && sleep 1 && kill -0 "${LUMEN_WESTON}" 2>/dev/null &&
    grep -aqE '^ +[0-9]+x[0-9]+@[0-9.]+.*, current' "${LOG}/weston-psc.log"
}

if [ "${COMPOSITOR}" != stock ]; then
  if [ -f "${PW_MARK}" ]; then   # the last attempt never showed a picture
    v=$(head -1 "${PW_MARK}" | tr -dc 0-9)
    [ -n "${v}" ] && mv -f "${PW_MARK}" "${LUMEN_HOME}/compositor/.failed-${v}" &&
      echo "psc compositor ${v}: the last attempt showed no picture, skipped from now on"
    rm -f "${PW_MARK}"; sync
  fi
  for v in 16 6; do
    [ -n "${STARTED}" ] && break
    tar=$(pw_tar "${v}") || continue
    [ -f "${LUMEN_HOME}/compositor/.failed-${v}" ] && { echo "psc compositor ${v}: skipped (.failed-${v})"; continue; }
    try_psc_weston "${v}" "${tar}" && STARTED=psc
  done
fi
if [ -z "${STARTED}" ] && [ "${DISPLAY_MODE}" = 1080p ]; then
  echo "display 1080p: restarting stock weston at 1920x1080"
  weston_custom "${LOG}/weston.log"
  if weston_up 6 && sleep 1 && grep -aq 'mode 1920x1080.*current' "${LOG}/weston.log"; then
    echo "weston 1080p up (${LUMEN_WESTON}): $(grep -a 'mode 1920x1080' "${LOG}/weston.log" | head -1)"
    STARTED=stock1080
  else
    echo "weston 1080p failed: $(grep -aE '^ +mode |failed to set mode|atomic commit failed' "${LOG}/weston.log" | head -3), back to 720p"
    DISPLAY_MODE=720p
  fi
fi
[ -n "${STARTED}" ] || { weston_stock; DISPLAY_MODE=720p; }
export LUMEN_DISPLAY="${DISPLAY_MODE:-720p}"
case "${STARTED}" in psc) export LUMEN_COMPOSITOR="weston${PW_VER}" ;; *) export LUMEN_COMPOSITOR=stock ;; esac
/usr/bin/wait_sdl

# console buttons: POWER suspends (handshake with the system power manager),
# RESET quits a running game. Only hand POWER to the system if the daemon runs.
rm -f /data/power/prepare_suspend /data/power/apps_okay
BUTTONS_PID=""
if [ -x "${LUMEN_HOME}/lumen-buttons" ] || chmod +x "${LUMEN_HOME}/lumen-buttons" 2>/dev/null; then
  "${LUMEN_HOME}/lumen-buttons" --log "${LOG}/buttons.log" > /tmp/lumen-buttons.out 2>&1 &
  BUTTONS_PID=$!
  for i in 1 2 3 4 5 6 7 8 9 10; do grep -q ready /tmp/lumen-buttons.out 2>/dev/null && break; sleep 0.1; done
fi
if [ -n "${BUTTONS_PID}" ] && grep -q ready /tmp/lumen-buttons.out 2>/dev/null; then
  echo 1 > /data/power/disable        # power manager suspends after our apps_okay
  echo "buttons daemon running (${BUTTONS_PID})"
else
  echo 2 > /data/power/disable        # no daemon: keep POWER ignored
  echo "buttons daemon not running"
fi

# 2. Lumen session
# the one-shot marker only guards the window until a frame is on screen: Lumen writes
# /tmp/lumen-presented after its first swap; a normal power-off later is not a failure
rm -f /tmp/lumen-presented
if [ -f "${PW_MARK}" ]; then
  ( i=0; while [ $i -lt 120 ]; do
      [ -f /tmp/lumen-presented ] && { rm -f "${PW_MARK}"; sync; echo "psc compositor: first frame presented"; exit 0; }
      sleep 1; i=$((i + 1)); done ) &
fi
sh "${LUMEN_HOME}/run.sh"
SESSION_RC=$?
echo "session ended: ${SESSION_RC}"
sync
if [ "${SESSION_RC}" = 12 ]; then   # Display settings changed: start over with the new setup
  echo "restart requested by the frontend"
  [ -n "${BUTTONS_PID}" ] && kill "${BUTTONS_PID}" 2>/dev/null
  sync; reboot; sleep 30
fi

# 3. stock Sony menu (internal games); power button handled by Sony's power manager
[ -n "${BUTTONS_PID}" ] && kill "${BUTTONS_PID}" 2>/dev/null
echo "starting stock menu"
killall -s TERM sdl_display 2>/dev/null
weston_stock   # the Sony menu is laid out for 720p
/usr/bin/wait_sdl
rm -f /data/power/prepare_suspend /data/power/apps_okay
echo 1 > /data/power/disable
sync
cd /data/AppData/sony/pcsx
export PCSX_ESC_KEY=2
/usr/sony/bin/ui_menu --power-off-enable >> "${LOG}/ui_menu.log" 2>&1
echo "stock menu exited: $?"
sync
