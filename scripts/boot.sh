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
weston_stock() {
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
DISPLAY_MODE=$(sed -n 's/^display=//p' "${LUMEN_HOME}/settings.ini" 2>/dev/null | tr -d '\r' | tail -1)
if [ "${DISPLAY_MODE}" = 1080p ] && [ -f /etc/xdg/weston/weston.ini ]; then
  echo "display 1080p: restarting weston at 1920x1080"
  sed "/^name=HDMI-A-1/{n;s/^mode=.*/mode=${MODE_1080P}/;}" /etc/xdg/weston/weston.ini > /tmp/lumen-weston.ini
  grep -A1 '^name=HDMI-A-1' /tmp/lumen-weston.ini
  systemctl stop weston.service
  killall -s KILL weston 2>/dev/null
  rm -f "${XDG_RUNTIME_DIR}/wayland-0" "${XDG_RUNTIME_DIR}/wayland-0.lock"
  /usr/bin/weston-launch -u root -- --idle-time=4294967 --tty=1 --config=/tmp/lumen-weston.ini \
    > "${LOG}/weston.log" 2>&1 &
  LUMEN_WESTON=$!
  if weston_up 6 && sleep 1 && grep -aq 'mode 1920x1080.*current' "${LOG}/weston.log"; then
    echo "weston 1080p up (${LUMEN_WESTON}): $(grep -a 'mode 1920x1080' "${LOG}/weston.log" | head -1)"
  else
    echo "weston 1080p failed: $(grep -aE '^ +mode |failed to set mode|atomic commit failed' "${LOG}/weston.log" | head -3), back to 720p"
    weston_stock; DISPLAY_MODE=720p
  fi
else
  weston_stock
fi
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
