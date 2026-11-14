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
LUMEN_WESTON=""
W6_TAR="${LUMEN_HOME}/weston6/weston6.tar.gz"
W6_ON=""; W6_SAMPLER=""
# One-shot guard: the marker is written before each attempt and removed when the
# session ends. Still there at boot = the last attempt ended in a power-off (black
# screen?): Weston 6 is skipped until the marker is deleted by hand.
W6_MARK="${LUMEN_HOME}/weston6/.trying"
# Experiment (compositor=weston6 in settings.ini and a Weston 6 build for the PSC at
# <home>/weston6/weston6.tar.gz): the newer compositor is unpacked to RAM and overlaid
# on /usr (overlayfs, nothing on the eMMC changes, gone at power-off). Its DRM backend
# can hand a fullscreen client buffer straight to the display instead of
# recompositing it with the GPU, which costs ~15 ms a frame at 1080p on the stock one.
weston6_off() {
  [ -n "${W6_ON}" ] || return 0
  [ -n "${W6_SAMPLER}" ] && kill "${W6_SAMPLER}" 2>/dev/null
  killall -s KILL weston 2>/dev/null; sleep 1
  if umount -l /usr; then rm -rf /tmp/lib_upper /tmp/lib_work; echo "weston6 overlay removed"
  else echo "weston6: umount /usr failed"; fi
  W6_ON=""; LUMEN_WESTON=""
}
weston_stock() {
  weston6_off
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
if [ "${COMPOSITOR}" = weston6 ] && [ -f "${W6_MARK}" ]; then
  echo "weston6 skipped: the previous attempt did not end cleanly (delete ${W6_MARK} to retry)"
elif [ "${COMPOSITOR}" = weston6 ] && [ -f "${W6_TAR}" ]; then
  echo "compositor weston6 (${DISPLAY_MODE:-720p}): overlay from RAM"
  : > "${W6_MARK}"; sync
  # Weston 6's desktop shell renamed the key that hides the panel
  sed -i '/^panel-location=none/a panel-position=none' /tmp/lumen-weston.ini
  systemctl stop weston.service; killall -s KILL weston 2>/dev/null
  mkdir -p /tmp/lib_upper/usr /tmp/lib_work/usr
  if tar xzpf "${W6_TAR}" -C /tmp/lib_upper/usr &&
     mount -t overlay overlay -o lowerdir=/usr,upperdir=/tmp/lib_upper/usr,workdir=/tmp/lib_work/usr /usr; then
    W6_ON=1
    weston_custom "${LOG}/weston6.log" --debug
    if weston_up 8 && sleep 1 && kill -0 "${LUMEN_WESTON}" 2>/dev/null &&
       ! grep -aqiE 'failed config|Cannot configure output|couldn.t commit new state|drmModeAddFB failed|PSC ERROR' "${LOG}/weston6.log"; then
      echo "weston6 up (${LUMEN_WESTON}): $(/usr/bin/weston --version 2>&1 | head -1)"
      grep -aiE 'mode |PSC|scanout|renderer' "${LOG}/weston6.log" | head -12
      STARTED=weston6
      grep -aqE 'mode 1920x1080.*current|1920x1080@' "${LOG}/weston6.log" || DISPLAY_MODE=720p
      # evidence: which plane each view lands on, sampled twice while the frontend runs
      ( sleep 14; [ -x /usr/bin/weston-debug ] || exit 0
        /usr/bin/weston-debug drm-backend > "${LOG}/weston6-planes.log" 2>&1 & d=$!; sleep 2; kill $d 2>/dev/null
        sleep 20; [ -x /usr/bin/weston-debug ] || exit 0
        /usr/bin/weston-debug drm-backend >> "${LOG}/weston6-planes.log" 2>&1 & d=$!; sleep 1; kill $d 2>/dev/null ) &
      W6_SAMPLER=$!
      # the frame may be at 720p if this build ignored the modeline: Lumen logs the real size
    else
      echo "weston6 failed to start: $(tail -5 "${LOG}/weston6.log" 2>/dev/null)"
      weston6_off
    fi
  else
    echo "weston6 overlay failed"; rm -rf /tmp/lib_upper /tmp/lib_work
  fi
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
sh "${LUMEN_HOME}/run.sh"
echo "session ended: $?"
rm -f "${W6_MARK}"; sync

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
